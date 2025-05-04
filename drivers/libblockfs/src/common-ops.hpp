#pragma once

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
async::result<helix::BorrowedDescriptor>
doAccessMemory(void *object) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	co_await inode->readyEvent.wait();
	co_return inode->accessMemory();
}

} // namespace blockfs
