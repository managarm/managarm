
#include <string.h>
#include <sys/epoll.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include "un-socket.hpp"

namespace un_socket {

bool logSockets = false;

struct Packet {
	// The actual octet data that the packet consists of.
	std::vector<char> buffer;

	std::vector<smarter::shared_ptr<File, FileHandle>> files;
};

struct OpenFile;
OpenFile *uniqueBind;

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
	: File{StructName::get("un-socket")}, _currentState{State::null}, _currentSeq{1}, _inSeq{0} { }

public:
	COFIBER_ROUTINE(expected<size_t>, readSome(void *data, size_t max_length) override, ([=] {
		assert(_currentState == State::connected);
		if(logSockets)
			std::cout << "posix: Read from socket " << this << std::endl;

		while(_recvQueue.empty())
			COFIBER_AWAIT _statusBell.async_wait();
		
		// TODO: Truncate packets (for SOCK_DGRAM) here.
		auto packet = &_recvQueue.front();
		assert(packet->files.empty());
		auto size = packet->buffer.size();
		assert(max_length >= size);
		memcpy(data, packet->buffer.data(), size);
		_recvQueue.pop_front();
		COFIBER_RETURN(size);
	}))
	
	COFIBER_ROUTINE(FutureMaybe<void>, writeAll(const void *data, size_t length) override, ([=] {
		assert(_currentState == State::connected);
		if(logSockets)
			std::cout << "posix: Write to socket " << this << std::endl;

		Packet packet;
		packet.buffer.resize(length);
		memcpy(packet.buffer.data(), data, length);

		_remote->_recvQueue.push_back(std::move(packet));
		_remote->_inSeq = ++_remote->_currentSeq;
		_remote->_statusBell.ring();

		COFIBER_RETURN();
	}))

	COFIBER_ROUTINE(FutureMaybe<RecvResult>, recvMsg(void *data, size_t max_length) override, ([=] {
		assert(_currentState == State::connected);
		if(logSockets)
			std::cout << "posix: Recv from socket \e[1;34m" << structName() << "\e[0m" << std::endl;

		while(_recvQueue.empty())
			COFIBER_AWAIT _statusBell.async_wait();
		
		// TODO: Truncate packets (for SOCK_DGRAM) here.
		auto packet = &_recvQueue.front();
		auto size = packet->buffer.size();
		assert(max_length >= size);
		memcpy(data, packet->buffer.data(), size);
		auto files = std::move(packet->files);
		_recvQueue.pop_front();
		COFIBER_RETURN(RecvResult(size, std::move(files)));
	}))
	
	COFIBER_ROUTINE(FutureMaybe<size_t>, sendMsg(const void *data, size_t max_length,
			std::vector<smarter::shared_ptr<File, FileHandle>> files), ([=] {
		assert(_currentState == State::connected);
		if(logSockets)
			std::cout << "posix: Send to socket \e[1;34m" << structName() << "\e[0m" << std::endl;

		Packet packet;
		packet.buffer.resize(max_length);
		memcpy(packet.buffer.data(), data, max_length);
		packet.files = std::move(files);

		_remote->_recvQueue.push_back(std::move(packet));
		_remote->_inSeq = ++_remote->_currentSeq;
		_remote->_statusBell.ring();

		COFIBER_RETURN(max_length);
	}))
	
	COFIBER_ROUTINE(async::result<AcceptResult>, accept() override, ([=] {
		assert(!_acceptQueue.empty());
		auto remote = std::move(_acceptQueue.front());
		_acceptQueue.pop_front();

		// Create a new socket and connect it to the queued one.
		auto local = smarter::make_shared<OpenFile>();
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
	
	COFIBER_ROUTINE(async::result<void>, bind(const void *, size_t) override, ([=] {
		assert(!uniqueBind);
		uniqueBind = this;
		COFIBER_RETURN();
	}))
	
	COFIBER_ROUTINE(async::result<void>, connect(const void *, size_t) override, ([=] {
		assert(uniqueBind);
		uniqueBind->_acceptQueue.push_back(this);
		uniqueBind->_inSeq = ++uniqueBind->_currentSeq;
		uniqueBind->_statusBell.ring();

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
};

smarter::shared_ptr<File, FileHandle> createSocketFile() {
	auto file = smarter::make_shared<OpenFile>();
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

std::array<smarter::shared_ptr<File, FileHandle>, 2> createSocketPair() {
	auto file0 = smarter::make_shared<OpenFile>();
	auto file1 = smarter::make_shared<OpenFile>();
	OpenFile::serve(file0);
	OpenFile::serve(file1);
	OpenFile::connectPair(file0.get(), file1.get());
	return {File::constructHandle(std::move(file0)), File::constructHandle(std::move(file1))};
}

} // namespace un_socket

