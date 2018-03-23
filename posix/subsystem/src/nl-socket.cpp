
#include <linux/netlink.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <iostream>
#include <map>

#include <async/doorbell.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include "nl-socket.hpp"

namespace nl_socket {

bool logSockets = true;

struct Packet {
	int senderPort;
	int group;

	// The actual octet data that the packet consists of.
	std::vector<char> buffer;
};

struct OpenFile : File {
public:
	static void serve(smarter::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		protocols::fs::servePassthrough(std::move(lane), smarter::shared_ptr<File>{file},
				&File::fileOperations);
	}

	OpenFile(int protocol)
	: File{StructName::get("nl-socket")}, _protocol{protocol},
			_currentSeq{1}, _inSeq{0} { }

	void deliver(Packet packet) {
		_recvQueue.push_back(std::move(packet));
		_inSeq = ++_currentSeq;
		_statusBell.ring();
	}

public:
	COFIBER_ROUTINE(expected<size_t>, readSome(void *data, size_t max_length) override, ([=] {
		if(logSockets)
			std::cout << "posix: Read from socket " << this << std::endl;

		while(_recvQueue.empty())
			COFIBER_AWAIT _statusBell.async_wait();
		
		// TODO: Truncate packets (for SOCK_DGRAM) here.
		auto packet = &_recvQueue.front();
		auto size = packet->buffer.size();
		assert(max_length >= size);
		memcpy(data, packet->buffer.data(), size);
		_recvQueue.pop_front();
		COFIBER_RETURN(size);
	}))
	
	COFIBER_ROUTINE(FutureMaybe<void>, writeAll(const void *data, size_t length) override, ([=] {
		throw std::runtime_error("posix: Fix netlink send()");
/*
		if(logSockets)
			std::cout << "posix: Write to socket " << this << std::endl;

		Packet packet;
		packet.buffer.resize(length);
		memcpy(packet.buffer.data(), data, length);

		_remote->deliver(std::move(packet));
*/

		COFIBER_RETURN();
	}))

	COFIBER_ROUTINE(FutureMaybe<RecvResult>,
	recvMsg(Process *process, void *data, size_t max_length,
			void *addr_ptr, size_t max_addr_length, size_t) override, ([=] {
		if(logSockets)
			std::cout << "posix: Recv from socket \e[1;34m" << structName() << "\e[0m" << std::endl;
		assert(max_addr_length >= sizeof(struct sockaddr_nl));

		while(_recvQueue.empty())
			COFIBER_AWAIT _statusBell.async_wait();
		
		// TODO: Truncate packets (for SOCK_DGRAM) here.
		auto packet = &_recvQueue.front();
		
		auto size = packet->buffer.size();
		assert(max_length >= size);
		memcpy(data, packet->buffer.data(), size);

		struct sockaddr_nl sa;
		memset(&sa, 0, sizeof(struct sockaddr_nl));
		sa.nl_family = AF_NETLINK;
		sa.nl_pid = packet->senderPort;
		sa.nl_groups = packet->group ? (1 << (packet->group - 1)) : 0;
		memcpy(addr_ptr, &sa, sizeof(struct sockaddr_nl));

		_recvQueue.pop_front();
		COFIBER_RETURN(RecvResult(size, sizeof(struct sockaddr_nl), std::vector<char>{}));
	}))
	
	COFIBER_ROUTINE(FutureMaybe<size_t>, sendMsg(const void *data, size_t max_length,
			std::vector<smarter::shared_ptr<File, FileHandle>> files), ([=] {
		throw std::runtime_error("posix: Fix netlink send()");
/*
		if(logSockets)
			std::cout << "posix: Send to socket \e[1;34m" << structName() << "\e[0m" << std::endl;

		Packet packet;
		packet.buffer.resize(max_length);
		memcpy(packet.buffer.data(), data, max_length);
		packet.files = std::move(files);

		_remote->deliver(std::move(packet));
*/

		COFIBER_RETURN(max_length);
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
		if(!_recvQueue.empty())
			events |= EPOLLIN;
		
//		std::cout << "posix: poll(" << past_seq << ") on \e[1;34m" << structName() << "\e[0m"
//				<< " returns (" << _currentSeq
//				<< ", " << edges << ", " << events << ")" << std::endl;
	
		COFIBER_RETURN(PollResult(_currentSeq, edges, events));
	}))
	
	async::result<void> bind(Process *, const void *, size_t) override;

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	int _protocol;
	helix::UniqueLane _passthrough;

	// Status management for poll().
	async::doorbell _statusBell;
	uint64_t _currentSeq;
	uint64_t _inSeq;
	
	// The actual receive queue of the socket.
	std::deque<Packet> _recvQueue;

	// For connected sockets, this is the socket we are connected to.
	OpenFile *_remote;
};

struct Group {
	friend struct OpenFile;

	void broadcast(std::string buffer);

private:
	std::vector<OpenFile *> _subscriptions;
};

std::map<std::pair<int, int>, std::unique_ptr<Group>> globalGroupMap;

// ----------------------------------------------------------------------------
// OpenFile implementation.
// ----------------------------------------------------------------------------

COFIBER_ROUTINE(async::result<void>,
OpenFile::bind(Process *, const void *addr_ptr, size_t addr_length), ([=] {
	struct sockaddr_nl sa;
	assert(addr_length <= sizeof(struct sockaddr_nl));
	memcpy(&sa, addr_ptr, addr_length);

	if(sa.nl_groups) {
		for(int i = 0; i < 32; i++) {
			if(!(sa.nl_groups & (1 << i)))
				continue;
			std::cout << "posix: Join netlink group "
					<< _protocol << "." << (i + 1) << std::endl;

			auto it = globalGroupMap.find({_protocol, i + 1});
			assert(it != globalGroupMap.end());
			auto group = it->second.get();
			group->_subscriptions.push_back(this);
		}
	}

	// Do nothing for now.
	COFIBER_RETURN();
}))

// ----------------------------------------------------------------------------
// Group implementation.
// ----------------------------------------------------------------------------

void Group::broadcast(std::string buffer) {
	for(auto socket : _subscriptions) {
		Packet packet;
		packet.senderPort = 0;
		packet.group = 1;
		packet.buffer.resize(buffer.size());
		memcpy(packet.buffer.data(), buffer.data(), buffer.size());

		socket->deliver(std::move(packet));
	}
}

// ----------------------------------------------------------------------------
// Free functions.
// ----------------------------------------------------------------------------

void configure(int protocol, int num_groups) {
	for(int i = 0; i < num_groups; i++) {
		std::pair<int, int> idx{protocol, i + 1};
		auto res = globalGroupMap.insert(std::make_pair(idx, std::make_unique<Group>()));
		assert(res.second);
	}
}

void broadcast(int proto_idx, int grp_idx, std::string buffer) {
	auto it = globalGroupMap.find({proto_idx, grp_idx});
	assert(it != globalGroupMap.end());
	auto group = it->second.get();
	group->broadcast(std::move(buffer));
}

smarter::shared_ptr<File, FileHandle> createSocketFile(int protocol) {
	auto file = smarter::make_shared<OpenFile>(protocol);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

} // namespace nl_socket

