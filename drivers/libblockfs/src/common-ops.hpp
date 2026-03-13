#pragma once

#include <async/algorithm.hpp>
#include <core/clock.hpp>
#include <frg/scope_exit.hpp>
#include "fs.hpp"
#include "trace.hpp"


namespace blockfs {

template <FileSystem T>
async::result<protocols::fs::SeekResult> doSeekAbs(void *object, int64_t offset) {
	using File = typename T::File;
	auto self = static_cast<File *>(object);

	co_await self->mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, self->mutex};

	self->offset = offset;

	co_return static_cast<ssize_t>(self->offset);
}

template <FileSystem T>
async::result<protocols::fs::SeekResult> doSeekRel(void *object, int64_t offset) {
	using File = typename T::File;
	auto self = static_cast<File *>(object);

	co_await self->mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, self->mutex};

	self->offset += offset;

	co_return static_cast<ssize_t>(self->offset);
}

template <FileSystem T>
async::result<protocols::fs::SeekResult> doSeekEof(void *object, int64_t offset) {
	using File = typename T::File;
	using Inode = typename T::Inode;
	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	co_await self->mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, self->mutex};

	co_await inode->readyEvent.wait();

	self->offset += offset + inode->fileSize();
	co_return static_cast<ssize_t>(self->offset);
}

template <FileSystem T>
async::result<protocols::fs::Error> doFlock(void *object, int flags) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	co_await inode->readyEvent.wait();

	auto result = co_await inode->flockManager.lock(&self->flock, flags);
	co_return result;
}

namespace detail {

template <Inode T>
async::result<protocols::fs::ReadResult> doReadImpl(T *inode, void *buffer, size_t length, auto &offset) {
	protocols::ostrace::Timer timer;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtRead,
			ostAttrNumBytes(length),
			ostAttrTime(timer.elapsed())
		);
	}};

	if (!length)
		co_return size_t{0};

	// TODO(geert): Pass cancellation token
	co_await inode->readyEvent.wait();

	if (inode->fileType == FileType::kTypeDirectory)
		co_return std::unexpected{protocols::fs::Error::isDirectory};
	if (offset >= inode->fileSize())
		co_return std::unexpected{protocols::fs::Error::endOfFile};

	auto remaining = inode->fileSize() - offset;
	auto chunkSize = std::min(length, remaining);
	if (!chunkSize)
		co_return std::unexpected{protocols::fs::Error::endOfFile};

	auto chunkOffset = offset;
	offset += chunkSize;

	// TODO: Add a sendFromMemory action to exchangeMsgs to avoid
	// having to copy this data twice.
	auto readMemory = co_await helix_ng::readMemory(
		inode->accessMemory(),
		chunkOffset, chunkSize, buffer);
	HEL_CHECK(readMemory.error());

	co_return chunkSize;
}

template <Inode T>
async::result<frg::expected<protocols::fs::Error, size_t>>
doWriteImpl(T *inode, const void *buffer, size_t length, bool append, auto &offset) {
	protocols::ostrace::Timer timer;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtWrite,
			ostAttrNumBytes(length),
			ostAttrTime(timer.elapsed())
		);
	}};

	if (!length)
		co_return size_t{0};

	co_await inode->readyEvent.wait();

	if (inode->fileType == FileType::kTypeDirectory)
		co_return protocols::fs::Error::isDirectory;

	if (append)
		offset = inode->fileSize();
	if (offset >= inode->fileSize())
		FRG_CO_TRY(co_await inode->resizeFile(offset + length));

	// TODO: Add a recvToMemory action to exchangeMsgs to avoid
	// having to copy this data twice.
	auto writeMemory = co_await helix_ng::writeMemory(
		inode->accessMemory(),
		offset, length, buffer);
	HEL_CHECK(writeMemory.error());

	offset += length;

	co_return length;
}


} // namespace detail


template <FileSystem T>
async::result<protocols::fs::ReadResult> doRead(void *object, helix_ng::CredentialsView,
		void *buffer, size_t length, async::cancellation_token) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	co_await self->mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, self->mutex};

	co_return co_await detail::doReadImpl(inode.get(), buffer, length, self->offset);
}


template <FileSystem T>
async::result<protocols::fs::ReadResult> doPread(void *object, int64_t offset, helix_ng::CredentialsView,
		void *buffer, size_t length) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	if (offset < 0)
		co_return std::unexpected{protocols::fs::Error::illegalArguments};
	size_t unsignedOffset = offset;

	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	co_await self->mutex.async_lock_shared();
	frg::shared_lock lock{frg::adopt_lock, self->mutex};

	co_return co_await detail::doReadImpl(inode.get(), buffer, length, unsignedOffset);
}


template <FileSystem T>
async::result<frg::expected<protocols::fs::Error, size_t>> doWrite(void *object, helix_ng::CredentialsView,
		const void *buffer, size_t length) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	if (!self->write)
		co_return protocols::fs::Error::badFileDescriptor;

	co_await self->mutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, self->mutex};

	co_return co_await detail::doWriteImpl(inode.get(), buffer, length, self->append, self->offset);
}

template <FileSystem T>
async::result<frg::expected<protocols::fs::Error, size_t>> doPwrite(void *object, int64_t offset, helix_ng::CredentialsView,
		const void *buffer, size_t length) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	if (offset < 0)
		co_return protocols::fs::Error::illegalArguments;
	size_t unsignedOffset = offset;

	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	co_await self->mutex.async_lock_shared();
	frg::shared_lock lock{frg::adopt_lock, self->mutex};

	co_return co_await detail::doWriteImpl(inode.get(), buffer, length, false, unsignedOffset);
}

template <FileSystem T>
async::result<frg::expected<protocols::fs::Error>> doTruncate(void *object, size_t size) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	co_await self->mutex.async_lock_shared();
	frg::shared_lock lock{frg::adopt_lock, self->mutex};

	co_await inode->readyEvent.wait();

	FRG_CO_TRY(co_await inode->resizeFile(size));

	co_return frg::success;
}

template <FileSystem T>
async::result<helix::BorrowedDescriptor>
doAccessMemory(void *object) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	co_await inode->readyEvent.wait();
	co_return inode->accessMemory();
}

template <FileSystem T>
async::result<void> doObstructLink(std::shared_ptr<void> object, std::string name) {
	using Inode = typename T::Inode;

	auto self = std::static_pointer_cast<Inode>(object);

	self->obstructedLinks.insert(name);
	co_return;
}

template <FileSystem T>
async::result<protocols::fs::TraverseLinksResult>
doTraverseLinks(std::shared_ptr<void> object, std::deque<std::string> components) {
	using Inode = typename T::Inode;
	using DirEntry = typename T::DirEntry;

	auto self = std::static_pointer_cast<Inode>(object);

	protocols::ostrace::Timer timer;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(ostEvtTraverseLinks, ostAttrTime(timer.elapsed()));
	}};

	std::optional<DirEntry> entry;
	std::shared_ptr<Inode> parent = self;
	size_t processedComponents = 0;

	std::vector<std::pair<std::shared_ptr<void>, int64_t>> nodes;

	while (!components.empty()) {
		auto component = components.front();
		components.pop_front();
		processedComponents++;

		if (component == "..") {
			if (parent == self)
				co_return std::make_tuple(
				    nodes, protocols::fs::FileType::directory, processedComponents
				);

			auto entry = FRG_CO_TRY(co_await parent->findEntry(".."));
			assert(entry);
			parent = std::static_pointer_cast<Inode>(self->fs.accessInode(entry->inode));
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

				parent = std::static_pointer_cast<Inode>(ino);
			}
		}
	}

	if (!entry)
		co_return protocols::fs::Error::fileNotFound;

	protocols::fs::FileType type;
	switch (entry->fileType) {
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

template <FileSystem T>
async::result<protocols::fs::OpenResult>
doOpen(std::shared_ptr<void> object, bool write, bool read, bool append) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	auto self = std::static_pointer_cast<Inode>(object);
	auto file = smarter::make_shared<File>(self, write, read, append);
	co_await self->readyEvent.wait();

	auto [localCtrl, remoteCtrl] = helix::createStream();
	auto [localPt, remotePt] = helix::createStream();

	co_await self->updateTimes(clk::getRealtime(), std::nullopt, std::nullopt);

	[] (smarter::shared_ptr<File> file, BaseFileSystem &fs, helix::UniqueLane localCtrl,
			helix::UniqueLane localPt) -> async::detached {
		auto fileOps = fs.fileOps();

		co_await async::race_and_cancel(
			[&](async::cancellation_token) {
				return protocols::fs::serveFile(std::move(localCtrl),
						file.get(), fileOps);
			},
			[&](async::cancellation_token ct) {
				return protocols::fs::servePassthrough(std::move(localPt),
						file, fileOps, ct);
			}
		);
	}(file, self->fs, std::move(localCtrl), std::move(localPt));

	co_return protocols::fs::OpenResult{std::move(remoteCtrl), std::move(remotePt)};
}

template <FileSystem T>
async::result<protocols::fs::Error> doUtimensat(std::shared_ptr<void> object,
		std::optional<timespec> atime, std::optional<timespec> mtime, timespec ctime) {
	using Inode = typename T::Inode;

	auto self = std::static_pointer_cast<Inode>(object);
	co_await self->readyEvent.wait();

	co_return co_await self->updateTimes(atime, mtime, ctime);
}

} // namespace blockfs
