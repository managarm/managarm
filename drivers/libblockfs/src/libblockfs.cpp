#include <stdio.h>
#include <string.h>
#include <iostream>
#include <algorithm>
#include <string>
#include <sys/epoll.h>
#include <linux/cdrom.h>
#include <linux/fs.h>

#include <core/clock.hpp>
#include <frg/scope_exit.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/ostrace/ostrace.hpp>

#include <blockfs.hpp>
#include "gpt.hpp"
#include "ext2fs.hpp"
#include "raw.hpp"
#include "fs.bragi.hpp"
#include <bragi/helpers-std.hpp>

namespace blockfs {

bool tracingInitialized = false;
bool clkInitialized = false;

constinit protocols::ostrace::Event ostEvtGetLink{"libblockfs.getLink"};
constinit protocols::ostrace::Event ostEvtTraverseLinks{"libblockfs.traverseLinks"};
constinit protocols::ostrace::Event ostEvtRead{"libblockfs.read"};
constinit protocols::ostrace::Event ostEvtReadDir{"libblockfs.readDir"};
constinit protocols::ostrace::Event ostEvtWrite{"libblockfs.write"};
constinit protocols::ostrace::Event ostEvtRawRead{"libblockfs.rawRead"};
constinit protocols::ostrace::Event ostEvtExt2ManageInode{"ext2.manageInode"};
constinit protocols::ostrace::Event ostEvtExt2ManageFile{"ext2.manageFile"};
constinit protocols::ostrace::UintAttribute ostAttrTime{"time"};
constinit protocols::ostrace::UintAttribute ostAttrNumBytes{"numBytes"};

protocols::ostrace::Vocabulary ostVocabulary{
	ostEvtGetLink,
	ostEvtTraverseLinks,
	ostEvtRead,
	ostEvtReadDir,
	ostEvtWrite,
	ostEvtRawRead,
	ostEvtExt2ManageInode,
	ostEvtExt2ManageFile,
	ostAttrTime,
	ostAttrNumBytes,
};

protocols::ostrace::Context ostContext{ostVocabulary};

namespace {

async::result<protocols::fs::SeekResult> seekAbs(void *object, int64_t offset) {
	auto self = static_cast<ext2fs::OpenFile *>(object);
	self->offset = offset;
	co_return static_cast<ssize_t>(self->offset);
}

async::result<protocols::fs::SeekResult> seekRel(void *object, int64_t offset) {
	auto self = static_cast<ext2fs::OpenFile *>(object);
	self->offset += offset;
	co_return static_cast<ssize_t>(self->offset);
}

async::result<protocols::fs::SeekResult> seekEof(void *object, int64_t offset) {
	auto self = static_cast<ext2fs::OpenFile *>(object);
	self->offset += offset + self->inode->fileSize();
	co_return static_cast<ssize_t>(self->offset);
}

using FlockManager = protocols::fs::FlockManager;
using Flock = protocols::fs::Flock;

async::result<protocols::fs::Error> flock(void *object, int flags) {
	auto self = static_cast<ext2fs::OpenFile *>(object);
	co_await self->inode->readyJump.wait();
	auto inode = self->inode;

	auto result = co_await inode->flockManager.lock(&self->flock, flags);
	co_return result;
}

async::result<protocols::fs::ReadResult> read(void *object, helix_ng::CredentialsView,
		void *buffer, size_t length) {
	if (!length)
		co_return size_t{0};

	protocols::ostrace::Timer timer;
	auto self = static_cast<ext2fs::OpenFile *>(object);

	if(self->inode->fileType == FileType::kTypeDirectory) {
		co_return protocols::fs::Error::isDirectory;
	}

	co_await self->inode->readyJump.wait();

	if(self->offset >= self->inode->fileSize())
		co_return size_t{0};

	auto remaining = self->inode->fileSize() - self->offset;
	auto chunkSize = std::min(length, remaining);
	if(!chunkSize)
		co_return size_t{0}; // TODO: Return an explicit end-of-file error?

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
			helix::BorrowedDescriptor(self->inode->frontalMemory),
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
		void *buffer, size_t length) {
	assert(length);

	protocols::ostrace::Timer timer;
	auto self = static_cast<ext2fs::OpenFile *>(object);
	co_await self->inode->readyJump.wait();

	if(self->offset >= self->inode->fileSize())
		co_return size_t{0};

	auto remaining = self->inode->fileSize() - offset;
	auto chunk_size = std::min(length, remaining);
	if(!chunk_size)
		co_return size_t{0}; // TODO: Return an explicit end-of-file error?

	auto chunk_offset = offset;
	auto map_offset = chunk_offset & ~size_t(0xFFF);
	auto map_size = (((chunk_offset & size_t(0xFFF)) + chunk_size + 0xFFF) & ~size_t(0xFFF));

	helix::LockMemoryView lock_memory;
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(self->inode->frontalMemory),
			&lock_memory, map_offset, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Map the page cache into the address space.
	helix::Mapping file_map{helix::BorrowedDescriptor{self->inode->frontalMemory},
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
	if(self->append) {
		self->offset = self->inode->fileSize();
	}
	co_await self->inode->fs.write(self->inode.get(), self->offset, buffer, length);
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
	co_await self->inode->fs.write(self->inode.get(), offset, buffer, length);
	co_return length;
}

async::result<helix::BorrowedDescriptor>
accessMemory(void *object) {
	auto self = static_cast<ext2fs::OpenFile *>(object);
	co_await self->inode->readyJump.wait();
	co_return self->inode->frontalMemory;
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
	co_await self->inode->fs.truncate(self->inode.get(), size);
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

constexpr protocols::fs::FileOperations fileOperations {
	.seekAbs      = &seekAbs,
	.seekRel      = &seekRel,
	.seekEof      = &seekEof,
	.read         = &read,
	.pread        = &pread,
	.write        = &write,
	.pwrite       = &pwrite,
	.readEntries  = &readEntries,
	.accessMemory = &accessMemory,
	.truncate     = &truncate,
	.flock        = &flock,
	.getFileFlags = &getFileFlags,
	.setFileFlags = &setFileFlags,
};

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
	auto file = smarter::make_shared<ext2fs::OpenFile>(self);
	co_await self->readyJump.wait();
	file->append = append;

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

async::result<void> deobstructLink(std::shared_ptr<void> object, std::string name) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	self->obstructedLinks.erase(name);
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
				co_return std::make_tuple(nodes, protocols::fs::FileType::unknown, 0);

			parent = self->fs.accessInode(FRG_CO_TRY(co_await parent->findEntry(".."))->inode);
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

				parent = ino;
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

constexpr protocols::fs::NodeOperations nodeOperations{
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
	.deobstructLink = &deobstructLink,
	.traverseLinks = &traverseLinks
};

async::result<protocols::fs::ReadResult> rawRead(void *object, helix_ng::CredentialsView,
		void *buffer, size_t length) {
	assert(length);

	uint64_t start;
	HEL_CHECK(helGetClock(&start));

	auto self = static_cast<raw::OpenFile *>(object);
	auto file_size = co_await self->rawFs->device->getSize();

	if(self->offset >= file_size)
		co_return size_t{0};

	auto remaining = file_size - self->offset;
	auto chunkSize = std::min(length, remaining);
	if(!chunkSize)
		co_return size_t{0}; // TODO: Return an explicit end-of-file error?

	auto chunk_offset = self->offset;
	self->offset += chunkSize;

	auto readMemory = co_await helix_ng::readMemory(
			helix::BorrowedDescriptor(self->rawFs->frontalMemory),
			chunk_offset, chunkSize, buffer);
	HEL_CHECK(readMemory.error());

	uint64_t end;
	HEL_CHECK(helGetClock(&end));

	ostContext.emit(
		ostEvtRawRead,
		ostAttrNumBytes(length),
		ostAttrTime(end - start)
	);

	co_return chunkSize;
}

async::result<protocols::fs::Error> rawFlock(void *object, int flags) {
	auto self = static_cast<raw::OpenFile*>(object);

	auto result = co_await self->rawFs->flockManager.lock(&self->flock, flags);
	co_return result;
}

async::result<protocols::fs::SeekResult> rawSeekAbs(void *object, int64_t offset) {
	auto self = static_cast<raw::OpenFile*>(object);
	self->offset = offset;
	co_return static_cast<ssize_t>(self->offset);
}

async::result<protocols::fs::SeekResult> rawSeekRel(void *object, int64_t offset) {
	auto self = static_cast<raw::OpenFile*>(object);
	self->offset += offset;
	co_return static_cast<ssize_t>(self->offset);
}

async::result<protocols::fs::SeekResult> rawSeekEof(void *object, int64_t offset) {
	auto self = static_cast<raw::OpenFile *>(object);
	auto size = co_await self->rawFs->device->getSize();
	self->offset = offset + size;
	co_return static_cast<ssize_t>(self->offset);
}
async::result<void> rawIoctl(void *object, uint32_t id, helix_ng::RecvInlineResult msg,
		helix::UniqueLane conversation) {
	auto self = static_cast<raw::OpenFile *>(object);

	if(id == managarm::fs::GenericIoctlRequest::message_id) {
		auto req = bragi::parse_head_only<managarm::fs::GenericIoctlRequest>(msg);
		assert(req);
		if (req->command() == CDROM_GET_CAPABILITY) {
			managarm::fs::GenericIoctlReply rsp;
			rsp.set_error(managarm::fs::Errors::NOT_A_TERMINAL);

			auto ser = rsp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		} else {
			co_await self->rawFs->device->handleIoctl(req.value(), std::move(conversation));
		}
	}
}

constexpr protocols::fs::FileOperations rawOperations {
	.seekAbs = rawSeekAbs,
	.seekRel = rawSeekRel,
	.seekEof = rawSeekEof,
	.read = rawRead,
	.ioctl = rawIoctl,
	.flock = rawFlock,
};

} // anonymous namespace

BlockDevice::BlockDevice(size_t sector_size, int64_t parent_id)
: size(0), sectorSize(sector_size), parentId(parent_id) { }

async::detached servePartition(helix::UniqueLane lane, gpt::Partition *partition, std::unique_ptr<raw::RawFs> rawFs) {
	std::cout << "unix device: Connection" << std::endl;

	// TODO(qookie): Generic file system type
	std::unique_ptr<ext2fs::FileSystem> fs;

	while(true) {
		auto [accept, recv_head] = co_await helix_ng::exchangeMsgs(
				lane,
				helix_ng::accept(
					helix_ng::recvInline()
				)
			);

		HEL_CHECK(accept.error());
		HEL_CHECK(recv_head.error());

		auto conversation = accept.descriptor();

		auto preamble = bragi::read_preamble(recv_head);
		if(preamble.error()) {
			std::cout << "libblockfs: error decoding preamble" << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}

		managarm::fs::CntRequest req;
		if (preamble.id() == managarm::fs::CntRequest::message_id) {
			auto o = bragi::parse_head_only<managarm::fs::CntRequest>(recv_head);
			if(!o) {
				std::cout << "libblockfs: error decoding CntRequest" << std::endl;
				auto [dismiss] = co_await helix_ng::exchangeMsgs(
					conversation, helix_ng::dismiss());
				HEL_CHECK(dismiss.error());
			}

			req = *o;
		}
		recv_head.reset();

		if(req.req_type() == managarm::fs::CntReqType::DEV_MOUNT) {
			// Mount the actual file system
			fs = std::make_unique<ext2fs::FileSystem>(partition);
			co_await fs->init();
			printf("ext2fs is ready!\n");

			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			protocols::fs::serveNode(std::move(local_lane), fs->accessRoot(),
					&nodeOperations);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else if(req.req_type() == managarm::fs::CntReqType::SB_CREATE_REGULAR) {
			auto inode = co_await fs->createRegular(req.uid(), req.gid());

			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			protocols::fs::serveNode(std::move(local_lane),
					inode, &nodeOperations);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_id(inode->number);
			resp.set_file_type(managarm::fs::FileType::REGULAR);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else if(preamble.id() == managarm::fs::RenameRequest::message_id) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::fs::RenameRequest>(recv_head, tail);

			if (!req) {
				std::cout << "libblockfs: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			auto oldInode = fs->accessInode(req->inode_source());
			auto newInode = fs->accessInode(req->inode_target());

			assert(!req->old_name().empty() && req->old_name() != "." && req->old_name() != "..");
			auto old_result = co_await oldInode->findEntry(req->old_name());
			if(!old_result) {
				managarm::fs::SvrResponse resp;
				assert(old_result.error() == protocols::fs::Error::notDirectory);
				resp.set_error(managarm::fs::Errors::NOT_DIRECTORY);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()));
				HEL_CHECK(send_resp.error());
				continue;
			}

			auto old_file = old_result.value();
			managarm::fs::SvrResponse resp;
			if(old_file) {
				auto result = co_await newInode->unlink(req->new_name());
				if(!result) {
					if(result.error() == protocols::fs::Error::fileNotFound) {
						// Ignored
					} else if(result.error() == protocols::fs::Error::directoryNotEmpty) {
						resp.set_error(managarm::fs::Errors::DIRECTORY_NOT_EMPTY);

						auto ser = resp.SerializeAsString();
						auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
							helix_ng::sendBuffer(ser.data(), ser.size()));
						HEL_CHECK(send_resp.error());
						continue;
					} else if(result.error() == protocols::fs::Error::notDirectory) {
						resp.set_error(managarm::fs::Errors::NOT_DIRECTORY);

						auto ser = resp.SerializeAsString();
						auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
							helix_ng::sendBuffer(ser.data(), ser.size()));
						HEL_CHECK(send_resp.error());
						continue;
					} else {
						// Handle error
						std::cout << "libblockfs: rename: unhandled error: " << (int)result.error() << std::endl;
						assert(result.error() == protocols::fs::Error::fileNotFound || result.error() == protocols::fs::Error::directoryNotEmpty || result.error() == protocols::fs::Error::notDirectory);
					}
				}
				co_await newInode->link(req->new_name(), old_file.value().inode, old_file.value().fileType);
			} else {
				resp.set_error(managarm::fs::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()));
				HEL_CHECK(send_resp.error());
				continue;
			}

			auto result = co_await oldInode->unlink(req->old_name());
			if(!result) {
				assert(result.error() == protocols::fs::Error::fileNotFound);
				resp.set_error(managarm::fs::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()));
				HEL_CHECK(send_resp.error());
				continue;
			}
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()));
			HEL_CHECK(send_resp.error());
		}else if(req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			auto file = smarter::make_shared<raw::OpenFile>(rawFs.get());
			async::detach(protocols::fs::servePassthrough(std::move(local_lane),
					file,
					&rawOperations));

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		} else if(preamble.id() == managarm::fs::GenericIoctlRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::fs::GenericIoctlRequest>(recv_head);

			if(!req) {
				std::cout << "libblockfs: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(req->command() == BLKGETSIZE64) {
				managarm::fs::GenericIoctlReply rsp;
				rsp.set_error(managarm::fs::Errors::SUCCESS);
				rsp.set_size(co_await partition->getSize());

				auto ser = rsp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
			} else {
				std::cout << "\e[31m" "libblockfs: Unknown ioctl() message with ID "
						<< req->command() << "\e[39m" << std::endl;

				auto [dismiss] = co_await helix_ng::exchangeMsgs(
					conversation, helix_ng::dismiss());
				HEL_CHECK(dismiss.error());
			}
		}else{
			std::cout << "Unexpected request type " + std::to_string((int)req.req_type()) << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
	}
}

async::detached serveDevice(helix::UniqueLane lane, std::unique_ptr<raw::RawFs> rawFs) {
	std::cout << "unix device: Connection" << std::endl;

	while(true) {
		auto [accept, recv_head] = co_await helix_ng::exchangeMsgs(
				lane,
				helix_ng::accept(
					helix_ng::recvInline()
				)
			);

		HEL_CHECK(accept.error());
		HEL_CHECK(recv_head.error());

		auto conversation = accept.descriptor();

		auto preamble = bragi::read_preamble(recv_head);
		if(preamble.error()) {
			std::cout << "libblockfs: error decoding preamble" << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}

		managarm::fs::CntRequest req;
		if (preamble.id() == managarm::fs::CntRequest::message_id) {
			auto o = bragi::parse_head_only<managarm::fs::CntRequest>(recv_head);
			if(!o) {
				std::cout << "libblockfs: error decoding CntRequest" << std::endl;
				auto [dismiss] = co_await helix_ng::exchangeMsgs(
					conversation, helix_ng::dismiss());
				HEL_CHECK(dismiss.error());
			}

			req = *o;
		}
		recv_head.reset();

		if(req.req_type() == managarm::fs::CntReqType::DEV_MOUNT) {
			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_OPERATION_TARGET);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor({})
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else if(req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			auto file = smarter::make_shared<raw::OpenFile>(rawFs.get());
			async::detach(protocols::fs::servePassthrough(std::move(local_lane),
					file,
					&rawOperations));

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		} else if(preamble.id() == managarm::fs::GenericIoctlRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::fs::GenericIoctlRequest>(recv_head);

			if(!req) {
				std::cout << "libblockfs: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(req->command() == BLKGETSIZE64) {
				managarm::fs::GenericIoctlReply rsp;
				rsp.set_error(managarm::fs::Errors::SUCCESS);
				rsp.set_size(co_await rawFs->device->getSize());

				auto ser = rsp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(send_resp.error());
			} else {
				std::cout << "\e[31m" "libblockfs: Unknown ioctl() message with ID "
						<< req->command() << "\e[39m" << std::endl;

				auto [dismiss] = co_await helix_ng::exchangeMsgs(
					conversation, helix_ng::dismiss());
				HEL_CHECK(dismiss.error());
			}
		}else{
			std::cout << "Unexpected request type " + std::to_string((int)req.req_type()) << " to device" << std::endl;
			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
	}
}

async::detached runDevice(BlockDevice *device) {
	if (!tracingInitialized) {
		co_await ostContext.create();
		tracingInitialized = true;
	}

	if(!clkInitialized)
		co_await clk::enumerateTracker();

	// TODO(qookie): Don't leak the table.
	// Currently it should be fine to leak it since neither it nor
	// the device gets deleted anyway.
	auto table = new gpt::Table(device);
	co_await table->parse();

	int64_t diskId = 0;
	{
		mbus_ng::Properties descriptor {
			{"unix.devtype", mbus_ng::StringItem{"block"}},
			{"unix.blocktype", mbus_ng::StringItem{"disk"}},
			{"unix.diskname-prefix", mbus_ng::StringItem{device->diskNamePrefix}},
			{"unix.diskname-suffix", mbus_ng::StringItem{device->diskNameSuffix}},
			{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(device->parentId)}}
		};

		auto entity = (co_await mbus_ng::Instance::global().createEntity(
					"disk", descriptor)).unwrap();
		diskId = entity.id();

		auto rawFs = std::make_unique<raw::RawFs>(device);
		co_await rawFs->init();

		// See comment in mbus_ng::~EntityManager as to why this is necessary.
		[] (mbus_ng::EntityManager entity, std::unique_ptr<raw::RawFs> rawFs) -> async::detached {
			while (true) {
				auto [localLane, remoteLane] = helix::createStream();

				// If this fails, too bad!
				(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

				serveDevice(std::move(localLane), std::move(rawFs));
			}
		}(std::move(entity), std::move(rawFs));
	}

	int partId = 0;
	for(size_t i = 0; i < table->numPartitions(); ++i) {
		auto type = table->getPartition(i).type();
		printf("Partition %lu, type: %.8X-%.4X-%.4X-%.2X%.2X-%.2X%.2X%.2X%.2X%.2X%.2X\n",
				i, type.a, type.b, type.c, type.d[0], type.d[1],
				type.e[0], type.e[1], type.e[2], type.e[3], type.e[4], type.e[5]);

		if(type == gpt::type_guids::managarmRootPartition)
			printf("  It's a Managarm root partition!\n");

		auto paritition = &table->getPartition(i);

		auto rawFs = std::make_unique<raw::RawFs>(paritition);
		co_await rawFs->init();

		// Create an mbus object for the partition.
		mbus_ng::Properties descriptor{
			{"unix.devtype", mbus_ng::StringItem{"block"}},
			{"unix.blocktype", mbus_ng::StringItem{"partition"}},
			{"unix.partid", mbus_ng::StringItem{std::to_string(partId++)}},
			{"unix.diskid", mbus_ng::StringItem{std::to_string(diskId)}},
			{"unix.partname-suffix", mbus_ng::StringItem{device->partNameSuffix}},
			{"drvcore.mbus-parent", mbus_ng::StringItem{std::to_string(paritition->parentId)}},
			{"unix.is-managarm-root", mbus_ng::StringItem{std::to_string(type == gpt::type_guids::managarmRootPartition)}}
		};

		auto entity = (co_await mbus_ng::Instance::global().createEntity(
					"partition", descriptor)).unwrap();

		[] (mbus_ng::EntityManager entity, gpt::Partition *partition, std::unique_ptr<raw::RawFs> rawFs) -> async::detached {
			while (true) {
				auto [localLane, remoteLane] = helix::createStream();

				// If this fails, too bad!
				(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

				servePartition(std::move(localLane), partition, std::move(rawFs));
			}
		}(std::move(entity), paritition, std::move(rawFs));
	}
}

} // namespace blockfs
