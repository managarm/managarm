#include <core/clock.hpp>
#include <async/result.hpp>
#include <fcntl.h>
#include <protocols/fs/server.hpp>
#include <frg/scope_exit.hpp>

#include "../trace.hpp"
#include "../common-ops.hpp"

#include "ext2fs.hpp"

namespace blockfs::ext2fs {

namespace {

async::result<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>>
readEntries(void *object) {
	auto self = static_cast<ext2fs::OpenFile *>(object);

	ostContext.emit(
		ostEvtReadDir
	);

	co_await self->mutex.async_lock();
	frg::unique_lock fileLock{frg::adopt_lock, self->mutex};

	co_await self->inode->inodeMutex.async_lock_shared();
	frg::shared_lock inodeLock{frg::adopt_lock, self->inode->inodeMutex};

	co_return co_await self->readEntries();
}

async::result<int> getFileFlags(void *object) {
	auto self = static_cast<ext2fs::OpenFile *>(object);
	int flags = 0;

	if(self->read && self->write)
		flags |= O_RDWR;
	else if(self->read)
		flags |= O_RDONLY;
	else if(self->write)
		flags |= O_WRONLY;

	co_return flags;
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

	co_await self->inodeMutex.async_lock_shared();
	frg::shared_lock inodeLock{frg::adopt_lock, self->inodeMutex};

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

async::result<std::expected<protocols::fs::GetLinkResult, protocols::fs::Error>> link(std::shared_ptr<void> object,
		std::string name, int64_t ino) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	co_await self->fs.topologyMutex.async_lock_shared();
	frg::shared_lock topologyLock{frg::adopt_lock, self->fs.topologyMutex};

	co_await self->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};

	// Reject hard links to directories to guarantee an acyclic directory tree.
	auto target = std::static_pointer_cast<ext2fs::Inode>(self->fs.accessInode(ino));
	co_await target->readyEvent.wait();
	if(target->fileType == kTypeDirectory)
		co_return std::unexpected{protocols::fs::Error::insufficientPermissions};

	// link() requires the target to be locked.
	co_await target->inodeMutex.async_lock();
	frg::unique_lock targetLock{frg::adopt_lock, target->inodeMutex};

	auto entry = co_await self->link(std::move(name), ino, kTypeRegular);
	if(!entry)
		co_return std::unexpected{entry.error()};

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

async::result<std::expected<void, protocols::fs::Error>> unlink(std::shared_ptr<void> object, std::string name) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	co_await self->fs.topologyMutex.async_lock_shared();
	frg::shared_lock topologyLock{frg::adopt_lock, self->fs.topologyMutex};

	co_await self->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};

	auto entry = co_await self->findEntry(name);
	if(!entry)
		co_return std::unexpected{entry.error()};
	if(!entry.value())
		co_return std::unexpected{protocols::fs::Error::fileNotFound};
	if(entry.value()->fileType == kTypeDirectory)
		co_return std::unexpected{protocols::fs::Error::isDirectory};

	// removeEntry() requires the target to be locked.
	auto target = std::static_pointer_cast<ext2fs::Inode>(self->fs.accessInode(entry.value()->inode));
	co_await target->inodeMutex.async_lock();
	frg::unique_lock targetLock{frg::adopt_lock, target->inodeMutex};

	auto result = co_await self->removeEntry(std::move(name));
	if (!result)
		co_return std::unexpected{result.error()};
	co_return {};
}

async::result<std::expected<void, protocols::fs::Error>> rmdir(std::shared_ptr<void> object, std::string name) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	co_await self->fs.topologyMutex.async_lock_shared();
	frg::shared_lock topologyLock{frg::adopt_lock, self->fs.topologyMutex};

	co_await self->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};

	auto entry = co_await self->findEntry(name);
	if(!entry)
		co_return std::unexpected{entry.error()};
	if(!entry.value())
		co_return std::unexpected{protocols::fs::Error::fileNotFound};
	if(entry.value()->fileType != kTypeDirectory)
		co_return std::unexpected{protocols::fs::Error::notDirectory};

	// isDirectoryEmpty() and removeEntry() require the target to be locked.
	auto target = std::static_pointer_cast<ext2fs::Inode>(self->fs.accessInode(entry.value()->inode));
	co_await target->inodeMutex.async_lock();
	frg::unique_lock targetLock{frg::adopt_lock, target->inodeMutex};

	auto isEmpty = FRG_CO_TRY(co_await target->isDirectoryEmpty());
	if(!isEmpty)
		co_return std::unexpected{protocols::fs::Error::directoryNotEmpty};

	auto result = co_await self->removeEntry(std::move(name));
	if (!result)
		co_return std::unexpected{result.error()};
	co_return {};
}

async::result<protocols::fs::FileStats>
getStats(std::shared_ptr<void> object) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	co_await self->readyEvent.wait();

	co_await self->inodeMutex.async_lock_shared();
	frg::shared_lock inodeLock{frg::adopt_lock, self->inodeMutex};

	protocols::fs::FileStats stats;
	stats.linkCount = self->diskInode()->linksCount;
	stats.fileSize = self->fileSize();
	stats.mode = self->diskInode()->mode & 0xFFF;
	stats.uid = self->diskInode()->uid;
	stats.gid = self->diskInode()->gid;
	stats.accessTime.tv_sec = self->diskInode()->atime;
	stats.dataModifyTime.tv_sec = self->diskInode()->mtime;;
	stats.anyChangeTime.tv_sec = self->diskInode()->ctime;

	co_return stats;
}

async::result<std::expected<std::string, protocols::fs::Error>> readSymlink(std::shared_ptr<void> object) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	co_await self->readyEvent.wait();

	// readlink() is only valid on symbolic links.
	if(self->fileType != kTypeSymlink)
		co_return std::unexpected{protocols::fs::Error::illegalArguments};

	co_await self->inodeMutex.async_lock_shared();
	frg::shared_lock inodeLock{frg::adopt_lock, self->inodeMutex};

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

async::result<std::expected<protocols::fs::MkdirResult, protocols::fs::Error>>
mkdir(std::shared_ptr<void> object, std::string name, uid_t uid, gid_t gid, mode_t mode) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	co_await self->fs.topologyMutex.async_lock_shared();
	frg::shared_lock topologyLock{frg::adopt_lock, self->fs.topologyMutex};

	co_await self->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};

	auto entry = co_await self->mkdir(std::move(name), uid, gid, mode);

	if(!entry)
		co_return std::unexpected{entry.error()};

	assert(entry->inode);
	co_return protocols::fs::MkdirResult{self->fs.accessInode(entry->inode), entry->inode};
}

async::result<std::expected<protocols::fs::SymlinkResult, protocols::fs::Error>>
symlink(std::shared_ptr<void> object, std::string name, std::string target) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	co_await self->fs.topologyMutex.async_lock_shared();
	frg::shared_lock topologyLock{frg::adopt_lock, self->fs.topologyMutex};

	co_await self->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};

	auto entry = co_await self->symlink(std::move(name), std::move(target));

	if(!entry)
		co_return std::unexpected{entry.error()};

	assert(entry->inode);
	co_return protocols::fs::SymlinkResult{self->fs.accessInode(entry->inode), entry->inode};
}

async::result<protocols::fs::Error> chmod(std::shared_ptr<void> object, int mode) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	co_await self->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};

	auto result = co_await self->chmod(mode);

	co_return result;
}

async::result<protocols::fs::Error> chown(std::shared_ptr<void> object, std::optional<uid_t> uid, std::optional<gid_t> gid) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	co_await self->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};

	auto result = co_await self->chown(uid, gid);

	co_return result;
}

async::result<std::expected<protocols::fs::GetLinkResult, protocols::fs::Error>>
getLinkOrCreate(std::shared_ptr<void> object, std::string name, mode_t mode, bool exclusive,
		uid_t uid, gid_t gid) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);

	co_await self->fs.topologyMutex.async_lock_shared();
	frg::shared_lock topologyLock{frg::adopt_lock, self->fs.topologyMutex};

	co_await self->inodeMutex.async_lock();
	frg::unique_lock inodeLock{frg::adopt_lock, self->inodeMutex};

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

	// Lock the new inode immediately as it is published by link() below.
	co_await inode->inodeMutex.async_lock();
	frg::unique_lock newInodeLock{frg::adopt_lock, inode->inodeMutex};

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
	.read         = &doRead<FileSystem>,
	.pread        = &doPread<FileSystem>,
	.write        = &doWrite<FileSystem>,
	.pwrite       = &doPwrite<FileSystem>,
	.readEntries  = &readEntries,
	.accessMemory = &doAccessMemory<FileSystem>,
	.truncate     = &doTruncate<FileSystem>,
	.flock        = &doFlock<FileSystem>,
	.getFileFlags = &getFileFlags,
	.setFileFlags = &setFileFlags,
};

constinit protocols::fs::NodeOperations nodeOperations{
	.getStats = &getStats,
	.getLink = &getLink,
	.link = &link,
	.unlink = &unlink,
	.rmdir = &rmdir,
	.open = &doOpen<FileSystem>,
	.readSymlink = &readSymlink,
	.mkdir = &mkdir,
	.symlink = &symlink,
	.chmod = &chmod,
	.chown = &chown,
	.utimensat = &doUtimensat<FileSystem>,
	.obstructLink = &doObstructLink<FileSystem>,
	.traverseLinks = &doTraverseLinks<FileSystem>,
	.getLinkOrCreate = &getLinkOrCreate
};

} // namespace blockfs::ext2fs
