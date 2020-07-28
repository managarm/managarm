
#include <string.h>
#include <future>

#include <sys/socket.h>
#include <helix/ipc.hpp>
#include "file.hpp"
#include "process.hpp"
#include "fs.bragi.hpp"

namespace {

constexpr bool logDestruction = false;

} // anonymous namespace

// --------------------------------------------------------
// File implementation.
// --------------------------------------------------------

async::result<protocols::fs::SeekResult>
File::ptSeekAbs(void *object, int64_t offset) {
	auto self = static_cast<File *>(object);
	auto result = co_await self->seek(offset, VfsSeek::absolute);
	if(!result) {
		assert(result.error() == Error::seekOnPipe);
		co_return protocols::fs::Error::seekOnPipe;
	}else{
		co_return result.value();
	}
}

async::result<protocols::fs::SeekResult>
File::ptSeekRel(void *object, int64_t offset) {
	auto self = static_cast<File *>(object);
	auto result = co_await self->seek(offset, VfsSeek::relative);
	if(!result) {
		assert(result.error() == Error::seekOnPipe);
		co_return protocols::fs::Error::seekOnPipe;
	}else{
		co_return result.value();
	}
}

async::result<protocols::fs::SeekResult>
File::ptSeekEof(void *object, int64_t offset) {
	auto self = static_cast<File *>(object);
	auto result = co_await self->seek(offset, VfsSeek::eof);
	if(!result) {
		assert(result.error() == Error::seekOnPipe);
		co_return protocols::fs::Error::seekOnPipe;
	}else{
		co_return result.value();
	}
}

async::result<protocols::fs::ReadResult>
File::ptRead(void *object, const char *credentials,
		void *buffer, size_t length) {
	auto self = static_cast<File *>(object);
	auto process = findProcessWithCredentials(credentials);
	auto result = co_await self->readSome(process.get(), buffer, length);
	if(!result) {
		switch(result.error()) {
		case Error::illegalOperationTarget:
			co_return protocols::fs::Error::illegalArguments;
		case Error::wouldBlock:
			co_return protocols::fs::Error::wouldBlock;
		default:
			assert(!"Unexpected error from readSome()");
			__builtin_unreachable();
		}
	}else{
		co_return result.value();
	}
}

async::result<void> File::ptWrite(void *object, const char *credentials,
		const void *buffer, size_t length) {
	auto self = static_cast<File *>(object);
	auto process = findProcessWithCredentials(credentials);
	auto result = co_await self->writeAll(process.get(), buffer, length);
	assert(result || "Unexpected error from writeAll()");
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

async::result<int> File::ptGetOption(void *object, int option) {
	auto self = static_cast<File *>(object);
	return self->getOption(option);
}

async::result<void> File::ptSetOption(void *object, int option, int value) {
	auto self = static_cast<File *>(object);
	return self->setOption(option, value);
}

async::result<protocols::fs::Error> File::ptBind(void *object,
		const char *credentials,
		const void *addr_ptr, size_t addr_length) {
	auto self = static_cast<File *>(object);
	auto process = findProcessWithCredentials(credentials);
	return self->bind(process.get(), addr_ptr, addr_length);
}

async::result<protocols::fs::Error> File::ptListen(void *object) {
	auto self = static_cast<File *>(object);
	return self->listen();
}

async::result<protocols::fs::Error> File::ptConnect(void *object,
		const char *credentials, const void *addr, size_t addr_len) {
	auto self = static_cast<File *>(object);
	auto process = findProcessWithCredentials(credentials);
	return self->connect(process.get(), addr, addr_len);
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

async::result<int> File::ptGetFileFlags(void *object) {
	auto self = static_cast<File *>(object);
	return self->getFileFlags();
}

async::result<void> File::ptSetFileFlags(void *object, int flags) {
	auto self = static_cast<File *>(object);
	return self->setFileFlags(flags);
}

async::result<frg::expected<protocols::fs::Error, size_t>> File::ptPeername(void *object, void *addr_ptr, size_t max_addr_length) {
	auto self = static_cast<File *>(object);
	return self->peername(addr_ptr, max_addr_length);
}

async::result<protocols::fs::RecvResult>
File::ptRecvMsg(void *object, const char *creds, uint32_t flags,
		void *data, size_t len,
		void *addr, size_t addr_len,
		size_t max_ctrl_len) {
	auto self = static_cast<File *>(object);
	auto process = findProcessWithCredentials(creds);
	return self->recvMsg(process.get(), flags,
			data, len,
			addr, addr_len,
			max_ctrl_len);
}

async::result<protocols::fs::SendResult>
File::ptSendMsg(void *object, const char *creds, uint32_t flags,
		void *data, size_t len,
		void *addr, size_t addr_len,
		std::vector<uint32_t> fds) {
	auto self = static_cast<File *>(object);
	auto process = findProcessWithCredentials(creds);

	if(flags & ~(MSG_DONTWAIT | MSG_CMSG_CLOEXEC | MSG_NOSIGNAL)) {
		std::cout << "\e[31mposix: Unknown SENDMSG flags: 0x" << std::hex << flags
			<< std::dec << "\e[39m" << std::endl;
		assert(!"Flags not implemented");
	}
	if(flags & MSG_NOSIGNAL) {
		static bool warned = false;
		if(!warned)
			std::cout << "\e[35mposix: Ignoring MSG_NOSIGNAL\e[39m" << std::endl;
		warned = true;
		flags &= ~MSG_NOSIGNAL;
	}


	std::vector<smarter::shared_ptr<File, FileHandle>> files;
	for(auto fd : fds) {
		auto file = process->fileContext()->getFile(fd);
		assert(file && "Illegal FD for SENDMSG cmsg");
		files.push_back(std::move(file));
	}

	return self->sendMsg(process.get(), flags,
			data, len,
			addr, addr_len,
			std::move(files));
}

File::~File() {
	// Nothing to do here.
	if(logDestruction)
		std::cout << "\e[37mposix \e[1;34m" << structName()
				<< "\e[0m\e[37m: File was destructed\e[39m" << std::endl;
}

bool File::isTerminal() {
	return _defaultOps & defaultIsTerminal;
}

async::result<frg::expected<Error>> File::readExactly(Process *process,
		void *data, size_t length) {
	size_t offset = 0;
	while(offset < length) {
		auto result = FRG_CO_TRY(co_await readSome(process,
				(char *)data + offset, length - offset));
		if(!result)
			co_return Error::wouldBlock;
		offset += result;
	}

	co_return {};
}

async::result<frg::expected<Error, size_t>> File::readSome(Process *, void *, size_t) {
	std::cout << "\e[35mposix \e[1;34m" << structName()
			<< "\e[0m\e[35m: File does not support read()\e[39m" << std::endl;
	co_return Error::illegalOperationTarget;
}

void File::handleClose() {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement handleClose()" << std::endl;
}

async::result<frg::expected<Error>> File::writeAll(Process *, const void *, size_t) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement writeAll()" << std::endl;
	throw std::runtime_error("posix: Object has no File::writeAll()");
}

async::result<ReadEntriesResult> File::readEntries() {
	throw std::runtime_error("posix: Object has no File::readEntries()");
}

async::result<protocols::fs::RecvResult>
File::recvMsg(Process *, uint32_t, void *, size_t,
		void *, size_t, size_t) {
	throw std::runtime_error("posix: Object has no File::recvMsg()");
}

async::result<protocols::fs::SendResult>
File::sendMsg(Process *, uint32_t,
		const void *, size_t,
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

async::result<frg::expected<Error, off_t>> File::seek(off_t, VfsSeek) {
	if(_defaultOps & defaultPipeLikeSeek) {
		async::promise<frg::expected<Error, off_t>> promise;
		promise.set_value(Error::seekOnPipe);
		return promise.async_get();
	}else{
		std::cout << "posix \e[1;34m" << structName()
				<< "\e[0m: Object does not implement seek()" << std::endl;
		throw std::runtime_error("posix: Object has no File::seek()");
	}
}

expected<PollResult> File::poll(Process *, uint64_t, async::cancellation_token) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement poll()" << std::endl;
	throw std::runtime_error("posix: Object has no File::poll()");
}

expected<PollResult> File::checkStatus(Process *process) {
	return poll(process, 0, async::cancellation_token{});
}

async::result<int> File::getOption(int) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement getOption()" << std::endl;
	throw std::runtime_error("posix: Object has no File::getOption()");
}

async::result<void> File::setOption(int, int) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement setOption()" << std::endl;
	throw std::runtime_error("posix: Object has no File::setOption()");
}

async::result<frg::expected<Error, AcceptResult>> File::accept(Process *) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement accept()" << std::endl;
	throw std::runtime_error("posix: Object has no File::accept()");
}

async::result<protocols::fs::Error> File::bind(Process *, const void *, size_t) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement bind()" << std::endl;
	throw std::runtime_error("posix: Object has no File::bind()");
}

async::result<protocols::fs::Error> File::connect(Process *, const void *, size_t) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement connect()" << std::endl;
	throw std::runtime_error("posix: Object has no File::connect()");
}

async::result<protocols::fs::Error> File::listen() {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement listen()" << std::endl;
	co_return protocols::fs::Error::none;
}

async::result<size_t> File::sockname(void *addr_ptr, size_t max_addr_length) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement sockname()" << std::endl;
	throw std::runtime_error("posix: Object has no File::sockname()");
}

FutureMaybe<helix::UniqueDescriptor> File::accessMemory() {
	// TODO: Return an error.
	throw std::runtime_error("posix: Object has no File::accessMemory()");
}

async::result<void> File::ioctl(Process *, managarm::fs::CntRequest,
		helix::UniqueLane) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement ioctl()" << std::endl;
	throw std::runtime_error("posix: Object has no File::ioctl()");
}

async::result<int> File::getFileFlags() {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement getFileFlags()" << std::endl;
	co_return 0;
}

async::result<void> File::setFileFlags(int flags) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement setFileFlags()" << std::endl;
	co_return;
}

async::result<frg::expected<protocols::fs::Error, size_t>> File::peername(void *addr_ptr, size_t max_addr_length) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement getPeerName()" << std::endl;
	throw std::runtime_error("posix: Object has no File::peername()");
}

