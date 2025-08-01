
#include <string.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <helix/ipc.hpp>
#include "file.hpp"
#include "process.hpp"

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
		co_return result.error() | protocols::fs::toFsProtoError;
	}else{
		co_return result.value();
	}
}

async::result<protocols::fs::SeekResult>
File::ptSeekRel(void *object, int64_t offset) {
	auto self = static_cast<File *>(object);
	auto result = co_await self->seek(offset, VfsSeek::relative);
	if(!result) {
		co_return result.error() | protocols::fs::toFsProtoError;
	}else{
		co_return result.value();
	}
}

async::result<protocols::fs::SeekResult>
File::ptSeekEof(void *object, int64_t offset) {
	auto self = static_cast<File *>(object);
	auto result = co_await self->seek(offset, VfsSeek::eof);
	if(!result) {
		co_return result.error() | protocols::fs::toFsProtoError;
	}else{
		co_return result.value();
	}
}

async::result<protocols::fs::ReadResult>
File::ptRead(void *object, helix_ng::CredentialsView credentials,
		void *buffer, size_t length, async::cancellation_token ce) {
	auto self = static_cast<File *>(object);
	auto process = findProcessWithCredentials(credentials);
	co_return (co_await self->readSome(process.get(), buffer, length, ce))
		.transform_error(protocols::fs::toFsProtoError);
}

async::result<protocols::fs::ReadResult>
File::ptPread(void *object, int64_t offset, helix_ng::CredentialsView credentials, void *buffer, size_t length) {
	auto self = static_cast<File *>(object);
	auto process = findProcessWithCredentials(credentials);
	co_return (co_await self->pread(process.get(), offset, buffer, length))
		.transform_error(protocols::fs::toFsProtoError);
}

async::result<frg::expected<protocols::fs::Error, size_t>> File::ptWrite(void *object, helix_ng::CredentialsView credentials,
		const void *buffer, size_t length) {
	auto self = static_cast<File *>(object);
	auto process = findProcessWithCredentials(credentials);
	auto result = co_await self->writeAll(process.get(), buffer, length);
	if(!result) {
		switch(result.error()) {
		case Error::noSpaceLeft:
			co_return protocols::fs::Error::noSpaceLeft;
		case Error::notConnected:
			co_return protocols::fs::Error::notConnected;
		case Error::illegalOperationTarget:
			co_return protocols::fs::Error::illegalOperationTarget;
		default:
			assert(!"Unexpected error from writeAll()");
			__builtin_unreachable();
		}
	}
	co_return result.value();
}

async::result<frg::expected<protocols::fs::Error, size_t>> File::ptPwrite(void *object, int64_t offset, helix_ng::CredentialsView credentials,
			const void *buffer, size_t length) {
	auto self = static_cast<File *>(object);
	auto process = findProcessWithCredentials(credentials);
	auto result = co_await self->pwrite(process.get(), offset, buffer, length);
	if(!result) {
		switch(result.error()) {
		case Error::noSpaceLeft:
			co_return protocols::fs::Error::noSpaceLeft;
		default:
			assert(!"Unexpected error from pwrite()");
			__builtin_unreachable();
		}
	}
	co_return result.value();
}

async::result<ReadEntriesResult> File::ptReadEntries(void *object) {
	auto self = static_cast<File *>(object);
	return self->readEntries();
}

async::result<frg::expected<protocols::fs::Error>> File::ptTruncate(void *object, size_t size) {
	auto self = static_cast<File *>(object);
	return self->truncate(size);
}

async::result<frg::expected<protocols::fs::Error>> File::ptAllocate(void *object,
		int64_t offset, size_t size) {
	auto self = static_cast<File *>(object);

	co_return co_await self->allocate(offset, size);
}

async::result<protocols::fs::Error> File::ptBind(void *object,
		helix_ng::CredentialsView credentials,
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
		helix_ng::CredentialsView credentials, const void *addr, size_t addr_len) {
	auto self = static_cast<File *>(object);
	auto process = findProcessWithCredentials(credentials);
	return self->connect(process.get(), addr, addr_len);
}

async::result<size_t> File::ptSockname(void *object, void *addr_ptr, size_t max_addr_length) {
	auto self = static_cast<File *>(object);
	return self->sockname(addr_ptr, max_addr_length);
}

async::result<void> File::ptIoctl(void *object, uint32_t id, helix_ng::RecvInlineResult msg,
		helix::UniqueLane conversation) {
	auto self = static_cast<File *>(object);
	return self->ioctl(nullptr, id, std::move(msg), std::move(conversation));
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

async::result<frg::expected<protocols::fs::Error, int>> File::ptGetSeals(void *object) {
	auto self = static_cast<File *>(object);
	co_return co_await self->getSeals();
}

async::result<frg::expected<protocols::fs::Error, int>> File::ptAddSeals(void *object, int seals) {
	auto self = static_cast<File *>(object);
	co_return co_await self->addSeals(seals);
}

async::result<protocols::fs::RecvResult>
File::ptRecvMsg(void *object, helix_ng::CredentialsView creds, uint32_t flags,
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

async::result<frg::expected<protocols::fs::Error, size_t>>
File::ptSendMsg(void *object, helix_ng::CredentialsView creds, uint32_t flags,
		void *data, size_t len,
		void *addr, size_t addr_len,
		std::vector<uint32_t> fds, struct ucred ucreds) {
	auto self = static_cast<File *>(object);
	auto process = findProcessWithCredentials(creds);

	if(flags & ~(MSG_DONTWAIT | MSG_CMSG_CLOEXEC | MSG_NOSIGNAL)) {
		std::cout << "\e[31mposix: Unknown SENDMSG flags: 0x" << std::hex << flags
			<< std::dec << "\e[39m" << std::endl;
		assert(!"Flags not implemented");
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
			std::move(files), ucreds);
}

async::result<helix::BorrowedDescriptor>
File::ptAccessMemory(void *object) {
	auto self = static_cast<File *>(object);
	co_return co_await self->accessMemory();
}

async::result<frg::expected<protocols::fs::Error>> File::ptSetSocketOption(void *object,
		int layer, int number, std::vector<char> optbuf) {
	auto self = static_cast<File *>(object);
	co_return co_await self->setSocketOption(layer, number, optbuf);
}

async::result<frg::expected<protocols::fs::Error>> File::ptGetSocketOption(void *object,
	helix_ng::CredentialsView creds, int layer, int number, std::vector<char> &optbuf) {
	auto self = static_cast<File *>(object);
	auto process = findProcessWithCredentials(creds);
	co_return co_await self->getSocketOption(process.get(), layer, number, optbuf);
}

async::result<protocols::fs::Error> File::ptShutdown(void *object, int how) {
	auto self = static_cast<File *>(object);
	co_return co_await self->shutdown(how);
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
		// readExactly() only used in exec() so pass empty
		// cancellation token.
		auto result = co_await readSome(process, (char *)data + offset, length - offset, {});
		// TODO(geert): This is really weird, this function should
		// return a protocols::fs::Error and just propogate the error we
		// get from readSome(). Practically it has no effect right now,
		// since exec() doesn't check for the error code, but it's still
		// highly confusing.
		if (!result.has_value())
			co_return Error::eof;
		offset += result.value();
	}

	co_return {};
}

async::result<std::expected<size_t, Error>>
File::readSome(Process *, void *, size_t, async::cancellation_token) {
	std::cout << "\e[35mposix \e[1;34m" << structName()
			<< "\e[0m\e[35m: File does not support read()\e[39m" << std::endl;
	co_return std::unexpected{Error::illegalOperationTarget};
}

async::result<std::expected<size_t, Error>> File::pread(Process *, int64_t, void *, size_t) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement pread()" << std::endl;
	co_return std::unexpected{Error::seekOnPipe};
}

void File::handleClose() {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement handleClose()" << std::endl;
}

async::result<frg::expected<Error, size_t>> File::writeAll(Process *, const void *, size_t) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement writeAll()" << std::endl;
	co_return Error::illegalOperationTarget;
}

async::result<frg::expected<Error, ControllingTerminalState *>> File::getControllingTerminal() {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement getControllingTerminal()\e[39m" << std::endl;
	co_return Error::notTerminal;
}

async::result<frg::expected<Error, size_t>> File::pwrite(Process *, int64_t, const void *, size_t) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement pwrite()" << std::endl;
	co_return Error::seekOnPipe;
}

async::result<ReadEntriesResult> File::readEntries() {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement readEntries()" << std::endl;
	throw std::runtime_error("posix: Object has no File::readEntries()");
}

async::result<protocols::fs::RecvResult>
File::recvMsg(Process *, uint32_t, void *, size_t,
		void *, size_t, size_t) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement recvMsg()" << std::endl;
	co_return protocols::fs::Error::illegalOperationTarget;
}

async::result<frg::expected<protocols::fs::Error, size_t>>
File::sendMsg(Process *, uint32_t,
		const void *, size_t,
		const void *, size_t,
		std::vector<smarter::shared_ptr<File, FileHandle>>, struct ucred) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement sendMsg()" << std::endl;
	co_return protocols::fs::Error::illegalOperationTarget;
}

async::result<frg::expected<protocols::fs::Error>> File::truncate(size_t) {
	std::cout << "\e[35mposix \e[1;34m" << structName()
			<< "\e[0m\e[35m: File does not support truncate()\e[39m" << std::endl;
	co_return protocols::fs::Error::illegalOperationTarget;
}

async::result<frg::expected<protocols::fs::Error>> File::allocate(int64_t, size_t) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement allocate()" << std::endl;
	co_return protocols::fs::Error::illegalOperationTarget;
}

async::result<frg::expected<Error, off_t>> File::seek(off_t, VfsSeek) {
	if(_defaultOps & defaultPipeLikeSeek) {
		co_return Error::seekOnPipe;
	}else{
		std::cout << "posix \e[1;34m" << structName()
				<< "\e[0m: Object does not implement seek()" << std::endl;
		co_return Error::illegalOperationTarget;
	}
}

expected<PollResult> File::poll(Process *, uint64_t, async::cancellation_token) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement poll()" << std::endl;
	co_return Error::illegalOperationTarget;
}

async::result<frg::expected<Error, PollWaitResult>> File::pollWait(Process *process,
		uint64_t sequence, int mask,
		async::cancellation_token cancellation) {
	while(true) {
		auto resultOrError = co_await poll(process, sequence, cancellation);

		if(auto error = std::get_if<Error>(&resultOrError); error)
			co_return *error;

		auto result = std::get<PollResult>(resultOrError);
		if((std::get<1>(result) & mask) || cancellation.is_cancellation_requested())
			co_return PollWaitResult{std::get<0>(result), std::get<1>(result)};

		// Mask was not satisfied.
		sequence = std::get<0>(result);
	}
}

async::result<frg::expected<Error, PollStatusResult>> File::pollStatus(Process *process) {
	auto resultOrError = co_await poll(process, 0);

	if(auto error = std::get_if<Error>(&resultOrError); error)
		co_return *error;

	auto result = std::get<PollResult>(resultOrError);
	co_return PollStatusResult{std::get<0>(result), std::get<2>(result)};
}

async::result<frg::expected<Error, AcceptResult>> File::accept(Process *) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement accept()" << std::endl;
	co_return Error::illegalOperationTarget;
}

async::result<protocols::fs::Error> File::bind(Process *, const void *, size_t) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement bind()" << std::endl;
	co_return protocols::fs::Error::illegalOperationTarget;
}

async::result<protocols::fs::Error> File::connect(Process *, const void *, size_t) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement connect()" << std::endl;
	co_return protocols::fs::Error::illegalOperationTarget;
}

async::result<protocols::fs::Error> File::listen() {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement listen()" << std::endl;
	co_return protocols::fs::Error::none;
}

async::result<size_t> File::sockname(void *, size_t) {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement sockname()" << std::endl;
	throw std::runtime_error("posix: Object has no File::sockname()");
}

FutureMaybe<helix::UniqueDescriptor> File::accessMemory() {
	// TODO: Return an error.
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement accessMemory()" << std::endl;
	throw std::runtime_error("posix: Object has no File::accessMemory()");
}

async::result<void> File::ioctl(Process *, uint32_t id, helix_ng::RecvInlineResult msg,
		helix::UniqueLane conversation) {
	(void) id;
	msg.reset();

	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement ioctl()" << std::endl;

	auto [dismiss] = co_await helix_ng::exchangeMsgs(
		conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
	co_return;
}

async::result<frg::expected<Error, std::string>>
File::ttyname() {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement ttyname()" << std::endl;
	co_return Error::notTerminal;
}

async::result<int> File::getFileFlags() {
	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement getFileFlags()" << std::endl;
	co_return 0;
}

async::result<void> File::setFileFlags(int flags) {
	(void) flags;

	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement setFileFlags()" << std::endl;
	co_return;
}

async::result<frg::expected<protocols::fs::Error, size_t>> File::peername(void *addr_ptr, size_t max_addr_length) {
	(void) addr_ptr;
	(void) max_addr_length;

	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement getPeerName()" << std::endl;
	co_return protocols::fs::Error::illegalOperationTarget;
}

async::result<frg::expected<protocols::fs::Error, int>> File::getSeals() {
	co_return protocols::fs::Error::illegalOperationTarget;
}

async::result<frg::expected<protocols::fs::Error, int>> File::addSeals(int seals) {
	(void) seals;
	co_return protocols::fs::Error::illegalOperationTarget;
}

async::result<frg::expected<protocols::fs::Error>> File::setSocketOption(int layer,
		int number, std::vector<char> optbuf) {
	(void) layer;
	(void) number;
	(void) optbuf;

	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement setSocketOption()" << std::endl;

	co_return protocols::fs::Error::illegalOperationTarget;
}

async::result<frg::expected<protocols::fs::Error>> File::getSocketOption(Process *process, int layer,
		int number, std::vector<char> &optbuf) {
	(void) process;
	(void) layer;
	(void) number;
	(void) optbuf;

	std::cout << "posix \e[1;34m" << structName()
			<< "\e[0m: Object does not implement getSocketOption()" << std::endl;

	co_return protocols::fs::Error::illegalOperationTarget;
}

async::result<std::string> File::getFdInfo() {
	co_return {};
}

async::result<protocols::fs::Error> File::shutdown(int how) {
	(void) how;
	std::cout << "posix \e[1;34m" << structName()
		<< "\e[0m: Object does not implement shutdown()" << std::endl;

	co_return protocols::fs::Error::notSocket;
}
