#pragma once

#include "fs.hpp"


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

	// TODO
	co_await inode->readyJump.wait();

	self->offset += offset + inode->fileSize();
	co_return static_cast<ssize_t>(self->offset);
}

template <FileSystem T>
async::result<protocols::fs::Error> doFlock(void *object, int flags) {
	using File = typename T::File;
	using Inode = typename T::Inode;

	auto self = static_cast<File *>(object);
	auto inode = std::static_pointer_cast<Inode>(self->inode);

	// TODO
	co_await inode->readyJump.wait();

	auto result = co_await inode->flockManager.lock(&self->flock, flags);
	co_return result;
}


} // namespace blockfs
