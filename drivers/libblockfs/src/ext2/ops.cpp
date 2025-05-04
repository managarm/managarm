#include <core/clock.hpp>
#include <async/result.hpp>
#include <protocols/fs/server.hpp>
#include <frg/scope_exit.hpp>

#include "../trace.hpp"
#include "../common-ops.hpp"

#include "ext2fs.hpp"

namespace blockfs::ext2fs {

extern protocols::fs::FileOperations fileOperations;

namespace {

using FlockManager = protocols::fs::FlockManager;
using Flock = protocols::fs::Flock;

async::result<protocols::fs::ReadResult> read(void *object, helix_ng::CredentialsView,
		void *buffer, size_t length, async::cancellation_token) {
	if (!length)
		co_return size_t{0};

	protocols::ostrace::Timer timer;
	auto self = static_cast<ext2fs::OpenFile *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	if(inode->fileType == FileType::kTypeDirectory) {
		co_return std::unexpected{protocols::fs::Error::isDirectory};
	}

	co_await inode->readyJump.wait();

	if(self->offset >= inode->fileSize())
		co_return std::unexpected{protocols::fs::Error::endOfFile};

	auto remaining = inode->fileSize() - self->offset;
	auto chunkSize = std::min(length, remaining);
	if(!chunkSize)
		co_return std::unexpected{protocols::fs::Error::endOfFile};

	auto chunk_offset = self->offset;
	self->offset += chunkSize;

	// TODO: If we *know* that the pages are already available,
	//       we can also fall back to the following "old" mapping code.
/*
	auto mapOffset = chunk_offset & ~size_t(0xFFF);
	auto mapSize = (((chunk_offset & size_t(0xFFF)) + chunkSize + 0xFFF) & ~size_t(0xFFF));

	helix::LockMemoryView lockMemory;
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(self->inode->frontalMemory),
			&lockMemory, mapOffset, mapSize, helix::Dispatcher::global());
	co_await submit.wait();
	HEL_CHECK(lockMemory.error());

	// Map the page cache into the address space.
	helix::Mapping fileMap{helix::BorrowedDescriptor{self->inode->frontalMemory},
			static_cast<ptrdiff_t>(mapOffset), mapSize,
			kHelMapProtRead | kHelMapDontRequireBacking};

	memcpy(buffer, reinterpret_cast<char *>(fileMap.get()) + (chunk_offset - mapOffset),
			chunkSize);
*/

	auto readMemory = co_await helix_ng::readMemory(
			helix::BorrowedDescriptor(inode->frontalMemory),
			chunk_offset, chunkSize, buffer);
	HEL_CHECK(readMemory.error());

	ostContext.emit(
		ostEvtRead,
		ostAttrNumBytes(length),
		ostAttrTime(timer.elapsed())
	);

	co_return chunkSize;
}

async::result<protocols::fs::ReadResult> pread(void *object, int64_t offset, helix_ng::CredentialsView,
		void *buffer, size_t length, async::cancellation_token) {
	assert(length);

	protocols::ostrace::Timer timer;
	auto self = static_cast<ext2fs::OpenFile *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	// TODO(geert): pass cancellation token
	co_await inode->readyJump.wait();

	if(self->offset >= inode->fileSize())
		co_return std::unexpected{protocols::fs::Error::endOfFile};

	auto remaining = inode->fileSize() - offset;
	auto chunk_size = std::min(length, remaining);
	if(!chunk_size)
		co_return std::unexpected{protocols::fs::Error::endOfFile};

	auto chunk_offset = offset;
	auto map_offset = chunk_offset & ~size_t(0xFFF);
	auto map_size = (((chunk_offset & size_t(0xFFF)) + chunk_size + 0xFFF) & ~size_t(0xFFF));

	helix::LockMemoryView lock_memory;
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(inode->frontalMemory),
			&lock_memory, map_offset, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Map the page cache into the address space.
	helix::Mapping file_map{helix::BorrowedDescriptor{inode->frontalMemory},
			static_cast<ptrdiff_t>(map_offset), map_size,
			kHelMapProtRead | kHelMapDontRequireBacking};

	memcpy(buffer, reinterpret_cast<char *>(file_map.get()) + (chunk_offset - map_offset),
			chunk_size);

	ostContext.emit(
		ostEvtRead,
		ostAttrNumBytes(length),
		ostAttrTime(timer.elapsed())
	);

	co_return chunk_size;
}

async::result<frg::expected<protocols::fs::Error, size_t>> write(void *object, helix_ng::CredentialsView,
		const void *buffer, size_t length) {
	if(!length) {
		co_return 0;
	}
	protocols::ostrace::Timer timer;

	auto self = static_cast<ext2fs::OpenFile *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);
	if(self->append) {
		self->offset = inode->fileSize();
	}
	co_await inode->fs.write(inode.get(), self->offset, buffer, length);
	self->offset += length;

	ostContext.emit(
		ostEvtWrite,
		ostAttrNumBytes(length),
		ostAttrTime(timer.elapsed())
	);

	co_return length;
}

async::result<frg::expected<protocols::fs::Error, size_t>> pwrite(void *object, int64_t offset, helix_ng::CredentialsView credentials,
			const void *buffer, size_t length) {
	(void) credentials;

	if(!length) {
		co_return 0;
	}

	auto self = static_cast<ext2fs::OpenFile *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);
	co_await inode->fs.write(inode.get(), offset, buffer, length);
	co_return length;
}

async::result<helix::BorrowedDescriptor>
accessMemory(void *object) {
	auto self = static_cast<ext2fs::OpenFile *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);
	co_await inode->readyJump.wait();
	co_return inode->frontalMemory;
}

async::result<protocols::fs::ReadEntriesResult>
readEntries(void *object) {
	auto self = static_cast<ext2fs::OpenFile *>(object);

	ostContext.emit(
		ostEvtReadDir
	);

	co_return co_await self->readEntries();
}

async::result<frg::expected<protocols::fs::Error>>
truncate(void *object, size_t size) {
	auto self = static_cast<ext2fs::OpenFile *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);
	co_await inode->fs.truncate(inode.get(), size);
	co_return {};
}

async::result<int> getFileFlags(void *) {
	std::cout << "libblockfs: getFileFlags is stubbed" << std::endl;
	co_return 0;
}

async::result<void> setFileFlags(void *, int) {
	std::cout << "libblockfs: setFileFlags is stubbed" << std::endl;
	co_return;
}


async::result<frg::expected<protocols::fs::Error, protocols::fs::GetLinkResult>>
getLink(std::shared_ptr<void> object,
		std::string name) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	protocols::ostrace::Timer timer;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtGetLink,
			ostAttrTime(timer.elapsed())
		);
	}};


	assert(!name.empty() && name != "." && name != "..");
	auto entry = FRG_CO_TRY(co_await self->findEntry(name));
	if(!entry)
		co_return protocols::fs::GetLinkResult{nullptr, -1,
				protocols::fs::FileType::unknown};

	protocols::fs::FileType type;
	switch(entry->fileType) {
	case kTypeDirectory:
		type = protocols::fs::FileType::directory;
		break;
	case kTypeRegular:
		type = protocols::fs::FileType::regular;
		break;
	case kTypeSymlink:
		type = protocols::fs::FileType::symlink;
		break;
	default:
		throw std::runtime_error("Unexpected file type");
	}

	assert(entry->inode);
	co_return protocols::fs::GetLinkResult{self->fs.accessInode(entry->inode), entry->inode, type};
}

async::result<protocols::fs::GetLinkResult> link(std::shared_ptr<void> object,
		std::string name, int64_t ino) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	auto entry = co_await self->link(std::move(name), ino, kTypeRegular);
	if(!entry)
		co_return protocols::fs::GetLinkResult{nullptr, -1,
				protocols::fs::FileType::unknown};

	protocols::fs::FileType type;
	switch(entry->fileType) {
	case kTypeDirectory:
		type = protocols::fs::FileType::directory;
		break;
	case kTypeRegular:
		type = protocols::fs::FileType::regular;
		break;
	case kTypeSymlink:
		type = protocols::fs::FileType::symlink;
		break;
	default:
		throw std::runtime_error("Unexpected file type");
	}

	assert(entry->inode);
	co_return protocols::fs::GetLinkResult{self->fs.accessInode(entry->inode), entry->inode, type};
}

async::result<frg::expected<protocols::fs::Error>> unlink(std::shared_ptr<void> object, std::string name) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	auto result = co_await self->unlink(std::move(name));
	if(!result) {
		assert(result.error() == protocols::fs::Error::fileNotFound
			|| result.error() == protocols::fs::Error::directoryNotEmpty);
		co_return result.error();
	}
	co_return {};
}

async::detached serve(smarter::shared_ptr<ext2fs::OpenFile> file,
		helix::UniqueLane local_ctrl, helix::UniqueLane local_pt) {
	async::cancellation_event cancel_pt;

	// Cancel the passthrough lane once the file line is closed.
	async::detach(protocols::fs::serveFile(std::move(local_ctrl),
			file.get(), &fileOperations), [&] {
		cancel_pt.cancel();
	});

	co_await protocols::fs::servePassthrough(std::move(local_pt),
			file, &fileOperations, cancel_pt);
}

async::result<protocols::fs::FileStats>
getStats(std::shared_ptr<void> object) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	co_await self->readyJump.wait();

	protocols::fs::FileStats stats;
	stats.linkCount = self->diskInode()->linksCount;
	stats.fileSize = self->fileSize();
	stats.mode = self->diskInode()->mode & 0xFFF;
	stats.uid = self->uid;
	stats.gid = self->gid;
	stats.accessTime.tv_sec = self->diskInode()->atime;
	stats.dataModifyTime.tv_sec = self->diskInode()->mtime;;
	stats.anyChangeTime.tv_sec = self->diskInode()->ctime;

	co_return stats;
}

async::result<protocols::fs::OpenResult>
open(std::shared_ptr<void> object, bool append) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	auto file = smarter::make_shared<ext2fs::OpenFile>(self, append);
	co_await self->readyJump.wait();

	helix::UniqueLane local_ctrl, remote_ctrl;
	helix::UniqueLane local_pt, remote_pt;
	std::tie(local_ctrl, remote_ctrl) = helix::createStream();
	std::tie(local_pt, remote_pt) = helix::createStream();
	struct timespec time = clk::getRealtime();
	self->diskInode()->atime = time.tv_sec;

	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			self->diskMapping.get(), self->fs.inodeSize);
	HEL_CHECK(syncInode.error());

	serve(file, std::move(local_ctrl), std::move(local_pt));

	co_return protocols::fs::OpenResult{std::move(remote_ctrl), std::move(remote_pt)};
}

async::result<std::string> readSymlink(std::shared_ptr<void> object) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	co_await self->readyJump.wait();

	if(self->fileSize() <= 60) {
		co_return std::string{self->diskInode()->data.embedded,
			self->diskInode()->data.embedded + self->fileSize()};
	} else {
		std::string result;
		result.resize(self->fileSize());
		co_await helix_ng::readMemory(
			helix::BorrowedDescriptor(self->frontalMemory),
			0, self->fileSize(), result.data());
		co_return result;
	}
}

async::result<protocols::fs::MkdirResult>
mkdir(std::shared_ptr<void> object, std::string name) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	auto entry = co_await self->mkdir(std::move(name));

	if(!entry)
		co_return protocols::fs::MkdirResult{nullptr, -1};

	assert(entry->inode);
	co_return protocols::fs::MkdirResult{self->fs.accessInode(entry->inode), entry->inode};
}

async::result<protocols::fs::SymlinkResult>
symlink(std::shared_ptr<void> object, std::string name, std::string target) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	auto entry = co_await self->symlink(std::move(name), std::move(target));

	if(!entry)
		co_return protocols::fs::SymlinkResult{nullptr, -1};

	assert(entry->inode);
	co_return protocols::fs::SymlinkResult{self->fs.accessInode(entry->inode), entry->inode};
}

async::result<protocols::fs::Error> chmod(std::shared_ptr<void> object, int mode) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	auto result = co_await self->chmod(mode);

	co_return result;
}

async::result<protocols::fs::Error> utimensat(std::shared_ptr<void> object,
		std::optional<timespec> atime, std::optional<timespec> mtime, timespec ctime) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	auto result = co_await self->utimensat(atime, mtime, ctime);

	co_return result;
}

async::result<void> obstructLink(std::shared_ptr<void> object, std::string name) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	self->obstructedLinks.insert(name);
	co_return;
}

async::result<protocols::fs::TraverseLinksResult> traverseLinks(std::shared_ptr<void> object,
		std::deque<std::string> components) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	protocols::ostrace::Timer timer;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtTraverseLinks,
			ostAttrTime(timer.elapsed())
		);
	}};

	std::optional<ext2fs::DirEntry> entry;
	std::shared_ptr<ext2fs::Inode> parent = self;
	size_t processedComponents = 0;

	std::vector<std::pair<std::shared_ptr<void>, int64_t>> nodes;

	while (!components.empty()) {
		auto component = components.front();
		components.pop_front();
		processedComponents++;

		if (component == "..") {
			if (parent == self)
				co_return std::make_tuple(nodes, protocols::fs::FileType::directory, processedComponents);

			auto entry = FRG_CO_TRY(co_await parent->findEntry(".."));
			assert(entry);
			parent = std::static_pointer_cast<ext2fs::Inode>(self->fs.accessInode(entry->inode));
			nodes.pop_back();
		} else {
			entry = FRG_CO_TRY(co_await parent->findEntry(component));

			if (!entry) {
				co_return protocols::fs::Error::fileNotFound;
			}

			assert(entry->inode);
			nodes.push_back({self->fs.accessInode(entry->inode), entry->inode});

			if (!components.empty()) {
				if (parent->obstructedLinks.find(component) != parent->obstructedLinks.end()) {
					break;
				}

				auto ino = self->fs.accessInode(entry->inode);
				if (entry->fileType == kTypeSymlink)
					break;

				if (entry->fileType != kTypeDirectory)
					co_return protocols::fs::Error::notDirectory;

				parent = std::static_pointer_cast<ext2fs::Inode>(ino);
			}
		}
	}

	if(!entry)
		co_return protocols::fs::Error::fileNotFound;

	protocols::fs::FileType type;
	switch(entry->fileType) {
	case kTypeDirectory:
		type = protocols::fs::FileType::directory;
		break;
	case kTypeRegular:
		type = protocols::fs::FileType::regular;
		break;
	case kTypeSymlink:
		type = protocols::fs::FileType::symlink;
		break;
	default:
		throw std::runtime_error("Unexpected file type");
	}

	co_return std::make_tuple(nodes, type, processedComponents);
}

async::result<std::expected<protocols::fs::GetLinkResult, protocols::fs::Error>>
getLinkOrCreate(std::shared_ptr<void> object, std::string name, mode_t mode, bool exclusive,
		uid_t uid, gid_t gid) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	auto findResult = co_await self->findEntry(name);

	if (!findResult)
		co_return std::unexpected{findResult.error()};

	auto result = findResult.value();

	if (result) {
		if (exclusive)
			co_return std::unexpected{protocols::fs::Error::alreadyExists};

		auto e = *result;
		protocols::fs::FileType type;
		switch(e.fileType) {
		case kTypeDirectory:
			type = protocols::fs::FileType::directory;
			break;
		case kTypeRegular:
			type = protocols::fs::FileType::regular;
			break;
		case kTypeSymlink:
			type = protocols::fs::FileType::symlink;
			break;
		default:
			throw std::runtime_error("Unexpected file type");
		}
		co_return protocols::fs::GetLinkResult{self->fs.accessInode(e.inode), e.inode, type};
	}

	auto baseInode = co_await self->fs.createRegular(uid, gid, self->number);
	auto inode = std::static_pointer_cast<ext2fs::Inode>(baseInode);
	auto chmodResult = co_await inode->chmod(mode);
	if (chmodResult != protocols::fs::Error::none)
		co_return std::unexpected{chmodResult};

	auto linkResult = co_await self->link(name, inode->number, FileType::kTypeRegular);
	if (!linkResult)
		co_return std::unexpected{protocols::fs::Error::internalError};

	co_return protocols::fs::GetLinkResult{inode, inode->number, protocols::fs::FileType::regular};
}

} // namespace anonymous

constinit protocols::fs::FileOperations fileOperations {
	.seekAbs      = &doSeekAbs<FileSystem>,
	.seekRel      = &doSeekRel<FileSystem>,
	.seekEof      = &doSeekEof<FileSystem>,
	.read         = &read,
	.pread        = &pread,
	.write        = &write,
	.pwrite       = &pwrite,
	.readEntries  = &readEntries,
	.accessMemory = &accessMemory,
	.truncate     = &truncate,
	.flock        = &doFlock<FileSystem>,
	.getFileFlags = &getFileFlags,
	.setFileFlags = &setFileFlags,
};

constinit protocols::fs::NodeOperations nodeOperations{
	.getStats = &getStats,
	.getLink = &getLink,
	.link = &link,
	.unlink = &unlink,
	.open = &open,
	.readSymlink = &readSymlink,
	.mkdir = &mkdir,
	.symlink = &symlink,
	.chmod = &chmod,
	.utimensat = &utimensat,
	.obstructLink = &obstructLink,
	.traverseLinks = &traverseLinks,
	.getLinkOrCreate = &getLinkOrCreate
};

} // namespace blockfs::ext2fs
