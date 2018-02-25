
#include <string.h>
#include <future>

#include <cofiber.hpp>
#include <cofiber/future.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>

#include "file.hpp"

// --------------------------------------------------------
// File implementation.
// --------------------------------------------------------

async::result<size_t> File::ptRead(std::shared_ptr<void> object,
		void *buffer, size_t length) {
	auto self = static_cast<File *>(object.get());
	return self->readSome(buffer, length);
}	

async::result<void> File::ptWrite(std::shared_ptr<void> object,
		const void *buffer, size_t length) {
	auto self = static_cast<File *>(object.get());
	return self->writeAll(buffer, length);
}

async::result<ReadEntriesResult> File::ptReadEntries(std::shared_ptr<void> object) {
	auto self = static_cast<File *>(object.get());
	return self->readEntries();
}

async::result<void> File::ptTruncate(std::shared_ptr<void> object, size_t size) {
	auto self = static_cast<File *>(object.get());
	return self->truncate(size);
}

async::result<void> File::ptAllocate(std::shared_ptr<void> object,
		int64_t offset, size_t size) {
	auto self = static_cast<File *>(object.get());
	return self->allocate(offset, size);
}

COFIBER_ROUTINE(FutureMaybe<void>, File::readExactly(void *data, size_t length), ([=] {
	size_t offset = 0;
	while(offset < length) {
		auto result = COFIBER_AWAIT readSome((char *)data + offset, length - offset);
		assert(result > 0);
		offset += result;
	}

	COFIBER_RETURN();
}))

FutureMaybe<void> File::writeAll(const void *, size_t) {
	throw std::runtime_error("posix: Object has no File::writeAll()");
}

async::result<ReadEntriesResult> File::readEntries() {
	throw std::runtime_error("posix: Object has no File::readEntries()");
}

FutureMaybe<RecvResult> File::recvMsg(void *, size_t) {
	throw std::runtime_error("posix: Object has no File::sendMsg()");
}

FutureMaybe<size_t> File::sendMsg(const void *, size_t,
		std::vector<std::shared_ptr<File>>) {
	throw std::runtime_error("posix: Object has no File::sendMsg()");
}

async::result<void> File::truncate(size_t) {
	throw std::runtime_error("posix: Object has no File::truncate()");
}

async::result<void> File::allocate(int64_t, size_t) {
	throw std::runtime_error("posix: Object has no File::allocate()");
}

FutureMaybe<off_t> File::seek(off_t, VfsSeek) {
	// TODO: Return an error.
	throw std::runtime_error("posix: Object has no File::seek()");
}

FutureMaybe<PollResult> File::poll(uint64_t) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement poll()" << std::endl;
	throw std::runtime_error("posix: Object has no File::poll()");
}

FutureMaybe<helix::UniqueDescriptor> File::accessMemory(off_t) {
	// TODO: Return an error.
	throw std::runtime_error("posix: Object has no File::accessMemory()");
}

