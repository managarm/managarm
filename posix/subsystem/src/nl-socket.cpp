
#include <string.h>
#include <sys/epoll.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include "nl-socket.hpp"

namespace nl_socket {

bool logSockets = false;

struct Packet {
	// The actual octet data that the packet consists of.
	std::vector<char> buffer;

	std::vector<std::shared_ptr<File>> files;
};

struct OpenFile : File {
	enum class State {
		null,
		connected
	};

public:
	static void connectPair(OpenFile *a, OpenFile *b) {
		assert(a->_state == State::null);
		assert(b->_state == State::null);
		a->_remote = b;
		b->_remote = a;
		a->_state = State::connected;
		b->_state = State::connected;
	}

	static void serve(std::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		protocols::fs::servePassthrough(std::move(lane), std::shared_ptr<File>{file},
				&File::fileOperations);
	}

	OpenFile()
	: File{StructName::get("nl-socket")}, _state{State::null}, _currentSeq{1}, _inSeq{0} { }

public:
	COFIBER_ROUTINE(FutureMaybe<size_t>, readSome(void *data, size_t max_length) override, ([=] {
		assert(_state == State::connected);
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
		assert(_state == State::connected);
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
		assert(_state == State::connected);
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
			std::vector<std::shared_ptr<File>> files), ([=] {
		assert(_state == State::connected);
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
	
	COFIBER_ROUTINE(FutureMaybe<PollResult>, poll(uint64_t past_seq) override, ([=] {
		assert(past_seq <= _currentSeq);
		while(past_seq == _currentSeq)
			COFIBER_AWAIT _statusBell.async_wait();

		// For now making sockets always writable is sufficient.
		int edges = EPOLLOUT;
		if(_inSeq > past_seq)
			edges |= EPOLLIN;

		int events = EPOLLOUT;
		if(!_recvQueue.empty())
			events |= EPOLLIN;
		
//		std::cout << "posix: poll(" << past_seq << ") on \e[1;34m" << structName() << "\e[0m"
//				<< " returns (" << _currentSeq
//				<< ", " << edges << ", " << events << ")" << std::endl;
	
		COFIBER_RETURN(PollResult(_currentSeq, edges, events));
	}))

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;

	State _state;

	// Status management for poll().
	async::doorbell _statusBell;
	uint64_t _currentSeq;
	uint64_t _inSeq;
	
	// The actual receive queue of the socket.
	std::deque<Packet> _recvQueue;

	// For connected sockets, this is the socket we are connected to.
	OpenFile *_remote;
};

std::shared_ptr<File> createSocketFile() {
	auto file = std::make_shared<OpenFile>();
	OpenFile::serve(file);
	return std::move(file);
}

std::array<std::shared_ptr<File>, 2> createSocketPair() {
	auto file0 = std::make_shared<OpenFile>();
	auto file1 = std::make_shared<OpenFile>();
	OpenFile::serve(file0);
	OpenFile::serve(file1);
	OpenFile::connectPair(file0.get(), file1.get());
	return {std::move(file0), std::move(file1)};
}

} // namespace nl_socket

