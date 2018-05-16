
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include "sockutil.hpp"
#include "un-socket.hpp"
#include "process.hpp"
#include "vfs.hpp"

namespace un_socket {

bool logSockets = false;

struct OpenFile;

// This map associates bound sockets with FS nodes.
// TODO: Use plain pointers instead of weak_ptrs and store a shared_ptr inside the OpenFile.
std::map<std::weak_ptr<FsNode>, OpenFile *,
		std::owner_less<std::weak_ptr<FsNode>>> globalBindMap;

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
		connected
	};

public:
	static void connectPair(OpenFile *a, OpenFile *b) {
		assert(a->_currentState == State::null);
		assert(b->_currentState == State::null);
		a->_remote = b;
		b->_remote = a;
		a->_currentState = State::connected;
		b->_currentState = State::connected;
		a->_stateBell.ring();
		b->_stateBell.ring();
	}

	static void serve(smarter::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		protocols::fs::servePassthrough(std::move(lane), smarter::shared_ptr<File>{file},
				&File::fileOperations);
	}

	OpenFile()
	: File{StructName::get("un-socket")}, _currentState{State::null},
			_currentSeq{1}, _inSeq{0}, _remote{nullptr}, _passCreds{false} { }

public:
	COFIBER_ROUTINE(expected<size_t>,
	readSome(Process *, void *data, size_t max_length) override, ([=] {
		assert(_currentState == State::connected);
		if(logSockets)
			std::cout << "posix: Read from socket " << this << std::endl;

		while(_recvQueue.empty())
			COFIBER_AWAIT _statusBell.async_wait();
		
		// TODO: Truncate packets (for SOCK_DGRAM) here.
		auto packet = &_recvQueue.front();
		assert(!packet->offset);
		assert(packet->files.empty());
		auto size = packet->buffer.size();
		assert(max_length >= size);
		memcpy(data, packet->buffer.data(), size);
		_recvQueue.pop_front();
		COFIBER_RETURN(size);
	}))
	
	COFIBER_ROUTINE(FutureMaybe<void>,
	writeAll(Process *process, const void *data, size_t length) override, ([=] {
		assert(process);
		assert(_currentState == State::connected);
		if(logSockets)
			std::cout << "posix: Write to socket " << this << std::endl;

		Packet packet;
		packet.senderPid = process->pid();
		packet.buffer.resize(length);
		memcpy(packet.buffer.data(), data, length);
		packet.offset = 0;

		_remote->_recvQueue.push_back(std::move(packet));
		_remote->_inSeq = ++_remote->_currentSeq;
		_remote->_statusBell.ring();

		COFIBER_RETURN();
	}))

	COFIBER_ROUTINE(expected<RecvResult>,
	recvMsg(Process *process, MsgFlags flags, void *data, size_t max_length,
			void *, size_t, size_t max_ctrl_length) override, ([=] {
		assert(_currentState == State::connected);
		assert(!(flags & ~(msgNoWait | msgCloseOnExec)));
		if(logSockets)
			std::cout << "posix: Recv from socket \e[1;34m" << structName() << "\e[0m" << std::endl;
		
		if(_recvQueue.empty() && (flags & msgNoWait)) {
			if(logSockets)
				std::cout << "posix: UNIX socket would block" << std::endl;
			COFIBER_RETURN(Error::wouldBlock);
		}

		while(_recvQueue.empty())
			COFIBER_AWAIT _statusBell.async_wait();
		
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
							flags & msgCloseOnExec));
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
		COFIBER_RETURN(RecvResult(chunk, 0, ctrl.buffer()));
	}))
	
	COFIBER_ROUTINE(expected<size_t>,
	sendMsg(Process *process, MsgFlags flags, const void *data, size_t max_length,
			const void *, size_t,
			std::vector<smarter::shared_ptr<File, FileHandle>> files), ([=] {
		assert(_currentState == State::connected);
		assert(!(flags & ~(msgNoWait)));
		if(logSockets)
			std::cout << "posix: Send to socket \e[1;34m" << structName() << "\e[0m" << std::endl;

		// We ignore msgNoWait here as we never block anyway.

		Packet packet;
		packet.senderPid = process->pid();
		packet.buffer.resize(max_length);
		memcpy(packet.buffer.data(), data, max_length);
		packet.files = std::move(files);
		packet.offset = 0;

		_remote->_recvQueue.push_back(std::move(packet));
		_remote->_inSeq = ++_remote->_currentSeq;
		_remote->_statusBell.ring();

		COFIBER_RETURN(max_length);
	}))
	
	COFIBER_ROUTINE(async::result<void>, setOption(int option, int value) override, ([=] {
		assert(option == SO_PASSCRED);
		_passCreds = value;
		COFIBER_RETURN();
	}));
	
	COFIBER_ROUTINE(async::result<AcceptResult>, accept() override, ([=] {
		assert(!_acceptQueue.empty());
		auto remote = std::move(_acceptQueue.front());
		_acceptQueue.pop_front();

		// Create a new socket and connect it to the queued one.
		auto local = smarter::make_shared<OpenFile>();
		local->setupWeakFile(local);
		OpenFile::serve(local);
		connectPair(remote, local.get());
		COFIBER_RETURN(File::constructHandle(std::move(local)));
	}))
	
	COFIBER_ROUTINE(expected<PollResult>, poll(uint64_t past_seq) override, ([=] {
		assert(past_seq <= _currentSeq);
		while(past_seq == _currentSeq)
			COFIBER_AWAIT _statusBell.async_wait();

		// For now making sockets always writable is sufficient.
		int edges = EPOLLOUT;
		if(_inSeq > past_seq)
			edges |= EPOLLIN;

		int events = EPOLLOUT;
		if(!_acceptQueue.empty() || !_recvQueue.empty())
			events |= EPOLLIN;
		
//		std::cout << "posix: poll(" << past_seq << ") on \e[1;34m" << structName() << "\e[0m"
//				<< " returns (" << _currentSeq
//				<< ", " << edges << ", " << events << ")" << std::endl;
	
		COFIBER_RETURN(PollResult(_currentSeq, edges, events));
	}))
	
	COFIBER_ROUTINE(async::result<void>,
	bind(Process *process, const void *addr_ptr, size_t addr_length) override, ([=] {
		// Create a new socket node in the FS.
		struct sockaddr_un sa;
		assert(addr_length <= sizeof(struct sockaddr_un));
		memcpy(&sa, addr_ptr, addr_length);

		std::string path{sa.sun_path, strnlen(sa.sun_path,
				addr_length - offsetof(sockaddr_un, sun_path))};
		std::cout << "posix: Bind to " << path << std::endl;

		PathResolver resolver;
		resolver.setup(process->fsContext()->getRoot(), std::move(path));
		COFIBER_AWAIT resolver.resolve(resolvePrefix);
		assert(resolver.currentLink());

		auto superblock = resolver.currentLink()->getTarget()->superblock();
		auto node = COFIBER_AWAIT superblock->createSocket();
		COFIBER_AWAIT resolver.currentLink()->getTarget()->link(resolver.nextComponent(), node);

		// Associate the current socket with the node.
		auto res = globalBindMap.insert({std::weak_ptr<FsNode>{node}, this});
		assert(res.second);

		COFIBER_RETURN();
	}))
	
	COFIBER_ROUTINE(async::result<void>,
	connect(Process *process, const void *addr_ptr, size_t addr_length) override, ([=] {
		// Resolve the socket node in the FS.
		struct sockaddr_un sa;
		assert(addr_length <= sizeof(struct sockaddr_un));
		memcpy(&sa, addr_ptr, addr_length);
		
		std::string path{sa.sun_path, strnlen(sa.sun_path,
				addr_length - offsetof(sockaddr_un, sun_path))};
		std::cout << "posix: Connect to " << path << std::endl;

		PathResolver resolver;
		resolver.setup(process->fsContext()->getRoot(), std::move(path));
		COFIBER_AWAIT resolver.resolve();
		assert(resolver.currentLink());

		// Lookup the socket associated with the node.
		auto node = resolver.currentLink()->getTarget();
		auto server = globalBindMap.at(node);
		server->_acceptQueue.push_back(this);
		server->_inSeq = ++server->_currentSeq;
		server->_statusBell.ring();

		while(_currentState == State::null)
			COFIBER_AWAIT _stateBell.async_wait();
		assert(_currentState == State::connected);
		std::cout << "posix: Connect returns" << std::endl;
		COFIBER_RETURN();
	}))
	
	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;

	async::doorbell _stateBell;
	State _currentState;

	// Status management for poll().
	async::doorbell _statusBell;
	uint64_t _currentSeq;
	uint64_t _inSeq;
	
	// TODO: Use weak_ptrs here!
	std::deque<OpenFile *> _acceptQueue;

	// The actual receive queue of the socket.
	std::deque<Packet> _recvQueue;

	// For connected sockets, this is the socket we are connected to.
	OpenFile *_remote;

	// Socket options.
	bool _passCreds;
};

smarter::shared_ptr<File, FileHandle> createSocketFile() {
	auto file = smarter::make_shared<OpenFile>();
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

std::array<smarter::shared_ptr<File, FileHandle>, 2> createSocketPair() {
	auto file0 = smarter::make_shared<OpenFile>();
	auto file1 = smarter::make_shared<OpenFile>();
	file0->setupWeakFile(file0);
	file1->setupWeakFile(file1);
	OpenFile::serve(file0);
	OpenFile::serve(file1);
	OpenFile::connectPair(file0.get(), file1.get());
	return {File::constructHandle(std::move(file0)), File::constructHandle(std::move(file1))};
}

} // namespace un_socket

