
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

COFIBER_ROUTINE(FutureMaybe<void>, File::readExactly(void *data, size_t length), ([=] {
	size_t offset = 0;
	while(offset < length) {
		auto result = COFIBER_AWAIT readSome((char *)data + offset, length - offset);
		assert(result > 0);
		offset += result;
	}

	COFIBER_RETURN();
}))

FutureMaybe<RecvResult> File::recvMsg(void *data, size_t max_length) {
	throw std::runtime_error("posix: Object has no File::sendMsg()");
}

FutureMaybe<size_t> File::sendMsg(const void *data, size_t max_length,
		std::vector<std::shared_ptr<File>> files) {
	throw std::runtime_error("posix: Object has no File::sendMsg()");
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

