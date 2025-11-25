#pragma once

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
async::result<protocols::fs::OpenResult>
doOpen(std::shared_ptr<void> object, bool append) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	auto self = std::static_pointer_cast<Inode>(object);
	auto file = smarter::make_shared<File>(self, append);
	co_await self->readyEvent.wait();

	auto [localCtrl, remoteCtrl] = helix::createStream();
	auto [localPt, remotePt] = helix::createStream();

	co_await self->updateTimes(clk::getRealtime(), std::nullopt, std::nullopt);

	[] (smarter::shared_ptr<File> file, BaseFileSystem &fs, helix::UniqueLane localCtrl,
			helix::UniqueLane localPt) -> async::detached {
		async::cancellation_event cancelPt;
		auto fileOps = fs.fileOps();

		// Cancel the passthrough lane once the file line is closed.
		async::detach(
			protocols::fs::serveFile(std::move(localCtrl),
					file.get(), fileOps),
			[&] {
				cancelPt.cancel();
			});

		co_await protocols::fs::servePassthrough(std::move(localPt),
				file, fileOps, cancelPt);
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
