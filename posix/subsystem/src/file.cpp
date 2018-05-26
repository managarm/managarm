
#include <string.h>
#include <future>

#include <cofiber.hpp>
#include <cofiber/future.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include "file.hpp"
#include "process.hpp"
#include "fs.pb.h"

namespace {

constexpr bool logDestruction = false;

} // anonymous namespace

// --------------------------------------------------------
// File implementation.
// --------------------------------------------------------

COFIBER_ROUTINE(async::result<protocols::fs::SeekResult>,
File::ptSeek(void *object, int64_t offset), ([=] {
	auto self = static_cast<File *>(object);
	auto result = COFIBER_AWAIT self->seek(offset, VfsSeek::relative);
	auto error = std::get_if<Error>(&result);
	if(error && *error == Error::seekOnPipe) {
		COFIBER_RETURN(protocols::fs::Error::seekOnPipe);
	}else{
		assert(!error);
		COFIBER_RETURN(std::get<off_t>(result));
	}
}))

COFIBER_ROUTINE(async::result<protocols::fs::ReadResult>,
File::ptRead(void *object, const char *credentials,
		void *buffer, size_t length), ([=] {
	auto self = static_cast<File *>(object);
	auto process = findProcessWithCredentials(credentials);
	auto result = COFIBER_AWAIT self->readSome(process.get(), buffer, length);
	auto error = std::get_if<Error>(&result);
	if(error && *error == Error::illegalOperationTarget) {
		COFIBER_RETURN(protocols::fs::Error::illegalArguments);
	}else{
		assert(!error);
		COFIBER_RETURN(std::get<size_t>(result));
	}
}))

async::result<void> File::ptWrite(void *object, const char *credentials,
		const void *buffer, size_t length) {
	auto self = static_cast<File *>(object);
	auto process = findProcessWithCredentials(credentials);
	return self->writeAll(process.get(), buffer, length);
}

async::result<ReadEntriesResult> File::ptReadEntries(void *object) {
	auto self = static_cast<File *>(object);
	return self->readEntries();
}

async::result<void> File::ptTruncate(void *object, size_t size) {
	auto self = static_cast<File *>(object);
	return self->truncate(size);
}

async::result<void> File::ptAllocate(void *object,
		int64_t offset, size_t size) {
	auto self = static_cast<File *>(object);
	return self->allocate(offset, size);
}

async::result<void> File::ptSetOption(void *object, int option, int value) {
	auto self = static_cast<File *>(object);
	return self->setOption(option, value);
}

async::result<void> File::ptBind(void *object, const char *credentials,
		const void *addr_ptr, size_t addr_length) {
	auto self = static_cast<File *>(object);
	auto process = findProcessWithCredentials(credentials);
	return self->bind(process.get(), addr_ptr, addr_length);
}

async::result<void> File::ptConnect(void *object, const char *credentials,
		const void *addr_ptr, size_t addr_length) {
	auto self = static_cast<File *>(object);
	auto process = findProcessWithCredentials(credentials);
	return self->connect(process.get(), addr_ptr, addr_length);
}

async::result<size_t> File::ptSockname(void *object, void *addr_ptr, size_t max_addr_length) {
	auto self = static_cast<File *>(object);
	return self->sockname(addr_ptr, max_addr_length);
}

async::result<void> File::ptIoctl(void *object, managarm::fs::CntRequest req,
		helix::UniqueLane conversation) {
	auto self = static_cast<File *>(object);
	return self->ioctl(nullptr, std::move(req), std::move(conversation));
}

COFIBER_ROUTINE(FutureMaybe<void>, File::readExactly(Process *process,
		void *data, size_t length), ([=] {
	size_t offset = 0;
	while(offset < length) {
		auto result = COFIBER_AWAIT readSome(process, (char *)data + offset, length - offset);
		assert(std::get<size_t>(result) > 0);
		offset += std::get<size_t>(result);
	}

	COFIBER_RETURN();
}))

File::~File() {
	// Nothing to do here.
	if(logDestruction)
		std::cout << "\e[37mposix \e[1;34m" << structName()
				<< "\e[0m\e[37m: File was destructed\e[39m" << std::endl;
}

COFIBER_ROUTINE(expected<size_t>, File::readSome(Process *, void *, size_t), ([=] {
	std::cout << "\e[35mposix \e[1;34m" << structName()
			<< "\e[0m\e[35m: File does not support read()\e[39m" << std::endl;
	COFIBER_RETURN(Error::illegalOperationTarget);
}))

void File::handleClose() {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement handleClose()" << std::endl;
}

FutureMaybe<void> File::writeAll(Process *, const void *, size_t) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement writeAll()" << std::endl;
	throw std::runtime_error("posix: Object has no File::writeAll()");
}

async::result<ReadEntriesResult> File::readEntries() {
	throw std::runtime_error("posix: Object has no File::readEntries()");
}

expected<RecvResult> File::recvMsg(Process *, MsgFlags, void *, size_t,
		void *, size_t, size_t) {
	throw std::runtime_error("posix: Object has no File::recvMsg()");
}

expected<size_t> File::sendMsg(Process *, MsgFlags, const void *, size_t,
		const void *, size_t,
		std::vector<smarter::shared_ptr<File, FileHandle>>) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement sendMsg()" << std::endl;
	throw std::runtime_error("posix: Object has no File::sendMsg()");
}

async::result<void> File::truncate(size_t) {
	throw std::runtime_error("posix: Object has no File::truncate()");
}

async::result<void> File::allocate(int64_t, size_t) {
	throw std::runtime_error("posix: Object has no File::allocate()");
}

expected<off_t> File::seek(off_t, VfsSeek) {
	if(_defaultOps & defaultPipeLikeSeek) {
		async::promise<std::variant<Error, off_t>> promise;
		promise.set_value(Error::seekOnPipe);
		return promise.async_get();
	}else{
		std::cout << "posix \e[1;34m" << structName()
				<< "\e[0m: Object does not implement seek()" << std::endl;
		throw std::runtime_error("posix: Object has no File::seek()");
	}
}

expected<PollResult> File::poll(Process *, uint64_t) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement poll()" << std::endl;
	throw std::runtime_error("posix: Object has no File::poll()");
}

expected<PollResult> File::checkStatus(Process *process) {
	return poll(process, 0);
}

async::result<void> File::setOption(int, int) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement setOption()" << std::endl;
	throw std::runtime_error("posix: Object has no File::setOption()");
}

async::result<AcceptResult> File::accept() {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement accept()" << std::endl;
	throw std::runtime_error("posix: Object has no File::accept()");
}

async::result<void> File::bind(Process *, const void *, size_t) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement bind()" << std::endl;
	throw std::runtime_error("posix: Object has no File::bind()");
}

async::result<void> File::connect(Process *, const void *, size_t) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement connect()" << std::endl;
	throw std::runtime_error("posix: Object has no File::connect()");
}

async::result<size_t> File::sockname(void *addr_ptr, size_t max_addr_length) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement sockname()" << std::endl;
	throw std::runtime_error("posix: Object has no File::sockname()");
}

FutureMaybe<helix::UniqueDescriptor> File::accessMemory(off_t) {
	// TODO: Return an error.
	throw std::runtime_error("posix: Object has no File::accessMemory()");
}

async::result<void> File::ioctl(Process *, managarm::fs::CntRequest,
		helix::UniqueLane) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement ioctl()" << std::endl;
	throw std::runtime_error("posix: Object has no File::ioctl()");
}

