
#include <string.h>
#include <sys/epoll.h>
#include <iostream>

#include <async/doorbell.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/server.hpp>
#include "socket.hpp"

namespace {

struct Packet {
	// The actual octet data that the packet consists of.
	std::vector<char> buffer;
};

// One direction of a socket.
struct Pipe {
	Pipe()
	: inSeq{0} { }

	Pipe(const Pipe &) = delete;

	Pipe &operator= (const Pipe &) = delete;

	std::deque<Packet> queue;
	async::doorbell bell;
	uint64_t inSeq;
};

// This is an actual socket.
// During normal operation, exactly two files are attached to it.
struct Socket {
	Socket()
	: currentSeq{1} { }

	Socket(const Socket &) = delete;

	Socket &operator= (const Socket &) = delete;

	Pipe pipes[2];
	async::doorbell seqBell;
	uint64_t currentSeq;
};

struct OpenFile : ProxyFile {
private:
	Pipe *_readPipe() {
		return &_socket->pipes[!_wh];
	}

	Pipe *_writePipe() {
		return &_socket->pipes[_wh];
	}

	// ------------------------------------------------------------------------
	// File protocol adapters.
	// ------------------------------------------------------------------------
private:
	static async::result<size_t> ptRead(std::shared_ptr<void> object,
			void *buffer, size_t length) {
		auto self = static_cast<OpenFile *>(object.get());
		return self->readSome(buffer, length);
	}
	
	static async::result<void> ptWrite(std::shared_ptr<void> object,
			const void *buffer, size_t length) {
		auto self = static_cast<OpenFile *>(object.get());
		return self->writeAll(buffer, length);
	}
	
	static constexpr auto fileOperations = protocols::fs::FileOperations{}
			.withRead(&ptRead)
			.withWrite(&ptWrite);

public:
	static void serve(std::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		protocols::fs::servePassthrough(std::move(lane), file,
				&fileOperations);
	}

	OpenFile()
	: ProxyFile{nullptr}, _wh{-1} { }
	
	OpenFile(std::shared_ptr<Socket> socket, int wh)
	: ProxyFile{nullptr}, _socket{std::move(socket)}, _wh{wh} { }

	COFIBER_ROUTINE(FutureMaybe<size_t>, readSome(void *data, size_t max_length) override, ([=] {
		std::cout << "posix: Read from socket " << this << std::endl;

		auto pipe = _readPipe();
		while(pipe->queue.empty())
			COFIBER_AWAIT pipe->bell.async_wait();
		
		std::cout << "posix: Completing read" << std::endl;
		
		// TODO: Truncate packets (for SOCK_DGRAM) here.
		auto packet = &pipe->queue.front();
		auto size = packet->buffer.size();
		assert(max_length >= size);
		memcpy(data, packet->buffer.data(), size);
		pipe->queue.pop_front();
		COFIBER_RETURN(size);
	}))
	
	COFIBER_ROUTINE(FutureMaybe<void>, writeAll(const void *data, size_t length), ([=] {
		assert(_socket);
		std::cout << "posix: Write to socket " << this << std::endl;

		Packet packet;
		packet.buffer.resize(length);
		memcpy(packet.buffer.data(), data, length);

		auto pipe = _writePipe();
		pipe->queue.push_back(std::move(packet));
		pipe->bell.ring();

		auto seq = ++_socket->currentSeq;
		pipe->inSeq = seq;
		_socket->seqBell.ring();

		COFIBER_RETURN();
	}))
	
	COFIBER_ROUTINE(FutureMaybe<PollResult>, poll(uint64_t in_seq) override, ([=] {
		if(!_socket) {
			std::cout << "posix: Fix poll() for unconnected sockets" << std::endl;
			return;
		}

		assert(in_seq <= _socket->currentSeq);
		while(in_seq == _socket->currentSeq)
			COFIBER_AWAIT _socket->seqBell.async_wait();

		// For now making sockets always writable is sufficient.
		int edges = EPOLLOUT;
		if(_readPipe()->inSeq > in_seq)
			edges |= EPOLLIN;

		int events = EPOLLOUT;
		if(!_readPipe()->queue.empty())
			events |= EPOLLIN;
		
		std::cout << "posix: poll(" << in_seq << ") on " << this
				<< " returns (" << _socket->currentSeq
				<< ", " << edges << ", " << events << ")" << std::endl;
	
		COFIBER_RETURN(PollResult(_socket->currentSeq, edges, events));
	}))

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;

	// Socket this file is connected to.
	// _wh is the index of the pipe this file writes to.
	std::shared_ptr<Socket> _socket;
	int _wh;
};

} // anonymous namespace

std::shared_ptr<ProxyFile> createUnixSocketFile() {
	auto file = std::make_shared<OpenFile>();
	OpenFile::serve(file);
	return std::move(file);
}

std::array<std::shared_ptr<ProxyFile>, 2> createUnixSocketPair() {
	auto socket = std::make_shared<Socket>();
	auto file0 = std::make_shared<OpenFile>(socket, 0);
	auto file1 = std::make_shared<OpenFile>(socket, 1);
	OpenFile::serve(file0);
	OpenFile::serve(file1);
	return {std::move(file0), std::move(file1)};
}

