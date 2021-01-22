
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <helix/ipc.hpp>
#include "sockutil.hpp"
#include "un-socket.hpp"
#include "process.hpp"
#include "vfs.hpp"

namespace un_socket {

static constexpr bool logSockets = false;

struct OpenFile;

// This map associates bound sockets with FS nodes.
// TODO: Use plain pointers instead of weak_ptrs and store a shared_ptr inside the OpenFile.
std::map<std::weak_ptr<FsNode>, OpenFile *,
		std::owner_less<std::weak_ptr<FsNode>>> globalBindMap;
std::unordered_map<std::string, OpenFile *> abstractSocketsBindMap;

struct Packet {
	// Sender process information.
	int senderPid;

	// The actual octet data that the packet consists of.
	std::vector<char> buffer;

	std::vector<smarter::shared_ptr<File, FileHandle>> files;

	size_t offset = 0;
};

struct OpenFile : File {
	enum class State {
		null,
		listening,
		connected,
		remoteShutDown,
		closed
	};

	enum class NameType {
		unnamed,
		path,
		abstract
	};

public:
	static void connectPair(OpenFile *a, OpenFile *b) {
		assert(a->_currentState == State::null);
		assert(b->_currentState == State::null);
		a->_remote = b;
		b->_remote = a;
		a->_currentState = State::connected;
		b->_currentState = State::connected;
		a->_statusBell.ring();
		b->_statusBell.ring();
	}

	static void serve(smarter::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				smarter::shared_ptr<File>{file}, &File::fileOperations, file->_cancelServe));
	}

	OpenFile(Process *process = nullptr, bool nonBlock = false)
	: File{StructName::get("un-socket"), File::defaultPipeLikeSeek}, _currentState{State::null},
			_currentSeq{1}, _inSeq{0}, _ownerPid{0},
			_remote{nullptr}, _passCreds{false}, nonBlock_{nonBlock},
			_sockpath{}, _nameType{NameType::unnamed}, _isInherited{false} {
		if(process)
			_ownerPid = process->pid();
	}

	void handleClose() override {
		if (!_isInherited && _nameType == NameType::abstract) {
			assert(abstractSocketsBindMap.find(_sockpath) != abstractSocketsBindMap.end());
			abstractSocketsBindMap.erase(_sockpath);
		}

		if(_currentState == State::connected) {
			auto rf = _remote;
			rf->_currentState = State::remoteShutDown;
			rf->_hupSeq = ++rf->_currentSeq;
			rf->_statusBell.ring();
			rf->_remote = nullptr;
			_remote = nullptr;
		}
		_currentState = State::closed;
		_statusBell.ring();
		_cancelServe.cancel();
	}

public:
	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t max_length) override {
		assert(_currentState == State::connected);
		if(logSockets)
			std::cout << "posix: Read from socket \e[1;34m" << structName() << "\e[0m" << std::endl;

		if(_recvQueue.empty() && nonBlock_) {
			if(logSockets)
				std::cout << "posix: UNIX socket would block" << std::endl;
			co_return Error::wouldBlock;
		}

		while(_recvQueue.empty())
			co_await _statusBell.async_wait();

		// TODO: Truncate packets (for SOCK_DGRAM) here.
		auto packet = &_recvQueue.front();
		assert(!packet->offset);
		assert(packet->files.empty());
		auto size = packet->buffer.size();
		assert(max_length >= size);
		memcpy(data, packet->buffer.data(), size);
		_recvQueue.pop_front();
		co_return size;
	}

	async::result<frg::expected<Error>>
	writeAll(Process *process, const void *data, size_t length) override {
		assert(process);
		if(_currentState != State::connected)
			co_return Error::notConnected;
		if(logSockets)
			std::cout << "posix: Write to socket \e[1;34m" << structName() << "\e[0m" << std::endl;

		Packet packet;
		packet.senderPid = process->pid();
		packet.buffer.resize(length);
		memcpy(packet.buffer.data(), data, length);
		packet.offset = 0;

		_remote->_recvQueue.push_back(std::move(packet));
		_remote->_inSeq = ++_remote->_currentSeq;
		_remote->_statusBell.ring();
		co_return {};
	}

	async::result<protocols::fs::RecvResult>
	recvMsg(Process *process, uint32_t flags, void *data, size_t max_length,
			void *, size_t, size_t max_ctrl_length) override {
		assert(!(flags & ~(MSG_DONTWAIT | MSG_CMSG_CLOEXEC)));

		if(_currentState == State::remoteShutDown)
			co_return protocols::fs::RecvResult { protocols::fs::RecvData { 0, 0, {} } };

		if(_currentState != State::connected)
			co_return protocols::fs::Error::notConnected;
		if(logSockets)
			std::cout << "posix: Recv from socket \e[1;34m" << structName() << "\e[0m" << std::endl;

		if(_recvQueue.empty() && ((flags & MSG_DONTWAIT) || nonBlock_)) {
			if(logSockets)
				std::cout << "posix: UNIX socket would block" << std::endl;
			co_return protocols::fs::RecvResult { protocols::fs::Error::wouldBlock };
		}

		while(_recvQueue.empty())
			co_await _statusBell.async_wait();

		auto packet = &_recvQueue.front();

		CtrlBuilder ctrl{max_ctrl_length};

		if(_passCreds) {
			struct ucred creds;
			memset(&creds, 0, sizeof(struct ucred));
			creds.pid = packet->senderPid;

			if(!ctrl.message(SOL_SOCKET, SCM_CREDENTIALS, sizeof(struct ucred)))
				throw std::runtime_error("posix: Implement CMSG truncation");
			ctrl.write<struct ucred>(creds);
		}

		if(!packet->files.empty()) {
			if(ctrl.message(SOL_SOCKET, SCM_RIGHTS, sizeof(int) * packet->files.size())) {
				for(auto &file : packet->files)
					ctrl.write<int>(process->fileContext()->attachFile(std::move(file),
							flags & MSG_CMSG_CLOEXEC));
			}else{
				throw std::runtime_error("posix: CMSG truncation is not implemented");
			}

			packet->files.clear();
		}

		// TODO: Truncate packets (for SOCK_DGRAM) here.
		auto chunk = std::min(packet->buffer.size() - packet->offset, max_length);
		memcpy(data, packet->buffer.data() + packet->offset, chunk);
		packet->offset += chunk;

		if(packet->offset == packet->buffer.size())
			_recvQueue.pop_front();
		co_return protocols::fs::RecvResult { protocols::fs::RecvData { chunk, 0, ctrl.buffer() } };
	}

	async::result<protocols::fs::SendResult>
	sendMsg(Process *process, uint32_t flags, const void *data, size_t max_length,
			const void *, size_t,
			std::vector<smarter::shared_ptr<File, FileHandle>> files) override {
		assert(!(flags & ~(MSG_DONTWAIT)));

		if(_currentState == State::remoteShutDown)
			co_return protocols::fs::SendResult { protocols::fs::Error::brokenPipe };

		if(_currentState != State::connected)
			co_return protocols::fs::Error::notConnected;
		if(logSockets)
			std::cout << "posix: Send to socket \e[1;34m" << structName() << "\e[0m" << std::endl;

		// We ignore MSG_DONTWAIT here as we never block anyway.

		Packet packet;
		packet.senderPid = process->pid();
		packet.buffer.resize(max_length);
		memcpy(packet.buffer.data(), data, max_length);
		packet.files = std::move(files);
		packet.offset = 0;

		_remote->_recvQueue.push_back(std::move(packet));
		_remote->_inSeq = ++_remote->_currentSeq;
		_remote->_statusBell.ring();

		co_return protocols::fs::SendResult { max_length };
	}

	async::result<int> getOption(int option) override {
		assert(option == SO_PEERCRED);
		if (_currentState != State::connected)
			co_return -1;
		co_return _remote->_ownerPid;
	}

	async::result<void> setOption(int option, int value) override {
		assert(option == SO_PASSCRED);
		_passCreds = value;
		co_return;
	}

	async::result<frg::expected<Error, AcceptResult>> accept(Process *process) override {
		if(_acceptQueue.empty() && nonBlock_) {
			if(logSockets)
				std::cout << "posix: UNIX socket would block on accept" << std::endl;
			co_return Error::wouldBlock;
		}

		while (!_acceptQueue.size())
			co_await _statusBell.async_wait();

		auto remote = std::move(_acceptQueue.front());
		_acceptQueue.pop_front();

		// Create a new socket and connect it to the queued one.
		auto local = smarter::make_shared<OpenFile>(process);
		local->_sockpath = _sockpath;
		local->_nameType = _nameType;
		local->_isInherited = true;
		local->setupWeakFile(local);
		OpenFile::serve(local);
		connectPair(remote, local.get());
		co_return File::constructHandle(std::move(local));
	}

	expected<PollResult> poll(Process *, uint64_t past_seq,
			async::cancellation_token cancellation) override {
		if(_currentState == State::closed)
			co_return Error::fileClosed;

		assert(past_seq <= _currentSeq);
		while(past_seq == _currentSeq && !cancellation.is_cancellation_requested())
			co_await _statusBell.async_wait(cancellation);

		if(_currentState == State::closed)
			co_return Error::fileClosed;

		// For now making sockets always writable is sufficient.
		int edges = EPOLLOUT;
		if(_hupSeq > past_seq)
			edges |= EPOLLHUP;
		if(_inSeq > past_seq)
			edges |= EPOLLIN;

		int events = EPOLLOUT;
		if(_currentState == State::remoteShutDown)
			events |= EPOLLHUP;
		if(!_acceptQueue.empty() || !_recvQueue.empty())
			events |= EPOLLIN;

//		std::cout << "posix: poll(" << past_seq << ") on \e[1;34m" << structName() << "\e[0m"
//				<< " returns (" << _currentSeq
//				<< ", " << edges << ", " << events << ")" << std::endl;

		co_return PollResult{_currentSeq, edges, events};
	}

	async::result<protocols::fs::Error>
	bind(Process *process, const void *addr_ptr, size_t addr_length) override {
		// Create a new socket node in the FS.
		struct sockaddr_un sa;
		assert(addr_length <= sizeof(struct sockaddr_un));
		memcpy(&sa, addr_ptr, addr_length);
		std::string path;

		if(sa.sun_path[0] == '\0') {
			path.resize(addr_length - sizeof(sa.sun_family) - 1);
			memcpy(path.data(), sa.sun_path + 1, addr_length - sizeof(sa.sun_family) - 1);
			_nameType = NameType::abstract;
		} else {
			path.resize(strnlen(sa.sun_path, addr_length - offsetof(sockaddr_un, sun_path)));
			memcpy(path.data(), sa.sun_path, strnlen(sa.sun_path, addr_length - offsetof(sockaddr_un, sun_path)));
			_nameType = NameType::path;
		}
		_sockpath = path;
		if(logSockets)
			std::cout << "posix: Bind to " << path << std::endl;

		if (_nameType == NameType::abstract) {
			auto res = abstractSocketsBindMap.emplace(path, this);
			if(!res.second)
				co_return protocols::fs::Error::addressInUse;
			co_return protocols::fs::Error::none;
		} else {
			PathResolver resolver;
			resolver.setup(process->fsContext()->getRoot(),
					process->fsContext()->getWorkingDirectory(), std::move(path));
			auto resolveResult = co_await resolver.resolve(resolvePrefix);
			if(!resolveResult) {
				co_return resolveResult.error();
			}
			assert(resolveResult);
			if(!resolver.currentLink())
				co_return protocols::fs::Error::fileNotFound;

			auto parentNode = resolver.currentLink()->getTarget();
			auto nodeResult = co_await parentNode->mksocket(resolver.nextComponent());
			if(!nodeResult) {
				co_return protocols::fs::Error::alreadyExists;
			}
			assert(nodeResult);
			auto node = nodeResult.value();
			// Associate the current socket with the node.
			auto res = globalBindMap.insert({std::weak_ptr<FsNode>{node->getTarget()}, this});
			if(!res.second)
				co_return protocols::fs::Error::addressInUse;
			co_return protocols::fs::Error::none;
		}
	}

	async::result<protocols::fs::Error>
	connect(Process *process, const void *addr_ptr, size_t addr_length) override {
		// Resolve the socket node in the FS.
		struct sockaddr_un sa;
		assert(addr_length <= sizeof(struct sockaddr_un));
		memcpy(&sa, addr_ptr, addr_length);
		std::string path;

		if(sa.sun_path[0] == '\0') {
			path.resize(addr_length - sizeof(sa.sun_family) - 1);
			memcpy(path.data(), sa.sun_path + 1, addr_length - sizeof(sa.sun_family) - 1);
		} else {
			path.resize(strnlen(sa.sun_path, addr_length - offsetof(sockaddr_un, sun_path)));
			memcpy(path.data(), sa.sun_path, strnlen(sa.sun_path, addr_length - offsetof(sockaddr_un, sun_path)));
		}

		if(logSockets)
			std::cout << "posix: Connect to " << path << std::endl;

		if (sa.sun_path[0] == '\0') {
			assert(!_ownerPid);
			_ownerPid = process->pid();

			auto server = abstractSocketsBindMap.at(path);
			server->_acceptQueue.push_back(this);
			server->_inSeq = ++server->_currentSeq;
			server->_statusBell.ring();

			while(_currentState == State::null)
				co_await _statusBell.async_wait();
			assert(_currentState == State::connected);
			co_return protocols::fs::Error::none;
		} else {
			PathResolver resolver;
			resolver.setup(process->fsContext()->getRoot(),
					process->fsContext()->getWorkingDirectory(), std::move(path));
			auto resolveResult = co_await resolver.resolve();
			if(!resolveResult) {
				co_return resolveResult.error();
			}
			assert(resolveResult);
			if(!resolver.currentLink())
				co_return protocols::fs::Error::fileNotFound;

			assert(!_ownerPid);
			_ownerPid = process->pid();

			// Lookup the socket associated with the node.
			auto node = resolver.currentLink()->getTarget();
			auto server = globalBindMap.at(node);
			server->_acceptQueue.push_back(this);
			server->_inSeq = ++server->_currentSeq;
			server->_statusBell.ring();

			while(_currentState == State::null)
				co_await _statusBell.async_wait();
			assert(_currentState == State::connected);
			co_return protocols::fs::Error::none;
		}
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	async::result<void> setFileFlags(int flags) override {
		if(flags & ~O_NONBLOCK) {
			std::cout << "posix: setFileFlags on socket \e[1;34m" << structName() << "\e[0m called with unknown flags" << std::endl;
			co_return;
		}
		if(flags & O_NONBLOCK)
			nonBlock_ = true;
		else
			nonBlock_ = false;
		co_return;
	}

	async::result<int> getFileFlags() override {
		if(nonBlock_)
			co_return O_NONBLOCK;
		co_return 0;
	}

private:
	static size_t getNameFor(OpenFile *sock, void *addrPtr, size_t maxAddrLength) {
		sockaddr_un sa;
		size_t outSize = offsetof(sockaddr_un, sun_path) + sock->_sockpath.size() + 1;

		memset(&sa, 0, sizeof(struct sockaddr_un));
		sa.sun_family = AF_UNIX;

		switch (sock->_nameType) {
			case NameType::unnamed:
				outSize = sizeof(sa_family_t);
				break;
			case NameType::abstract:
				sa.sun_path[0] = '\0';
				memcpy(sa.sun_path + 1, sock->_sockpath.data(),
						std::min(sizeof(sa.sun_path) - 1, sock->_sockpath.size()));
				break;
			case NameType::path:
				strncpy(sa.sun_path, sock->_sockpath.data(), sizeof(sa.sun_path));
				break;
		}

		auto destSize = std::min(sizeof(sockaddr_un), maxAddrLength);
		memcpy(addrPtr, &sa, destSize);

		return outSize;
	}

public:
	async::result<frg::expected<protocols::fs::Error, size_t>>
	peername(void *addrPtr, size_t maxAddrLength) override {
		if (_currentState != State::connected) {
			co_return protocols::fs::Error::notConnected;
		}

		co_return getNameFor(_remote, addrPtr, maxAddrLength);
	}

	async::result<size_t> sockname(void *addrPtr, size_t maxAddrLength) override {
		co_return getNameFor(this, addrPtr, maxAddrLength);
	}

private:
	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	State _currentState;

	// Status management for poll().
	async::doorbell _statusBell;
	uint64_t _currentSeq;
	uint64_t _hupSeq = 0;
	uint64_t _inSeq;

	// TODO: Use weak_ptrs here!
	std::deque<OpenFile *> _acceptQueue;

	// The actual receive queue of the socket.
	std::deque<Packet> _recvQueue;

	int _ownerPid;

	// For connected sockets, this is the socket we are connected to.
	OpenFile *_remote;

	// Socket options.
	bool _passCreds;
	bool nonBlock_;

	std::string _sockpath;

	NameType _nameType;

	bool _isInherited;
};

smarter::shared_ptr<File, FileHandle> createSocketFile(bool nonBlock) {
	auto file = smarter::make_shared<OpenFile>(nullptr, nonBlock);
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

std::array<smarter::shared_ptr<File, FileHandle>, 2> createSocketPair(Process *process) {
	auto file0 = smarter::make_shared<OpenFile>(process);
	auto file1 = smarter::make_shared<OpenFile>(process);
	file0->setupWeakFile(file0);
	file1->setupWeakFile(file1);
	OpenFile::serve(file0);
	OpenFile::serve(file1);
	OpenFile::connectPair(file0.get(), file1.get());
	return {File::constructHandle(std::move(file0)), File::constructHandle(std::move(file1))};
}

} // namespace un_socket

