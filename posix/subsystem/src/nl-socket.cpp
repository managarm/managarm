
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
#include "process.hpp"
#include "sockutil.hpp"

namespace nl_socket {

struct OpenFile;
struct Group;

bool logSockets = true;

std::map<std::pair<int, int>, std::unique_ptr<Group>> globalGroupMap;

int nextPort = -1;
std::map<int, OpenFile *> globalPortMap;

struct Packet {
	// Sender netlink socket information.
	int senderPort;
	int group;

	// Sender process information.
	int senderPid;

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
			_currentSeq{1}, _inSeq{0}, _socketPort{0}, _passCreds{false} { }

	void deliver(Packet packet) {
		_recvQueue.push_back(std::move(packet));
		_inSeq = ++_currentSeq;
		_statusBell.ring();
	}

public:
	COFIBER_ROUTINE(expected<size_t>,
	readSome(Process *, void *data, size_t max_length) override, ([=] {
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
	
	COFIBER_ROUTINE(FutureMaybe<void>,
	writeAll(Process *, const void *data, size_t length) override, ([=] {
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

	COFIBER_ROUTINE(expected<RecvResult>,
	recvMsg(Process *process, MsgFlags flags, void *data, size_t max_length,
			void *addr_ptr, size_t max_addr_length, size_t max_ctrl_length) override, ([=] {
		if(logSockets)
			std::cout << "posix: Recv from socket \e[1;34m" << structName() << "\e[0m" << std::endl;
		assert(!flags);
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
		
		CtrlBuilder ctrl{max_ctrl_length};

		if(_passCreds) {
			struct ucred creds;
			memset(&creds, 0, sizeof(struct ucred));
			creds.pid = packet->senderPid;

			if(!ctrl.message(SOL_SOCKET, SCM_CREDENTIALS, sizeof(struct ucred)))
				throw std::runtime_error("posix: Implement CMSG truncation");
			ctrl.write<struct ucred>(creds);
		}

		_recvQueue.pop_front();
		COFIBER_RETURN(RecvResult(size, sizeof(struct sockaddr_nl), ctrl.buffer()));
	}))
	
	expected<size_t> sendMsg(Process *process, MsgFlags flags,
			const void *data, size_t max_length,
			const void *addr_ptr, size_t addr_length,
			std::vector<smarter::shared_ptr<File, FileHandle>> files) override;
	
	COFIBER_ROUTINE(async::result<void>, setOption(int option, int value) override, ([=] {
		assert(option == SO_PASSCRED);
		_passCreds = value;
		COFIBER_RETURN();
	}));
	
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
	
	async::result<size_t> sockname(void *, size_t) override;

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	void _associatePort() {
		assert(!_socketPort);
		_socketPort = nextPort--;
		auto res = globalPortMap.insert({_socketPort, this});
		assert(res.second);
	}

	int _protocol;
	helix::UniqueLane _passthrough;

	// Status management for poll().
	async::doorbell _statusBell;
	uint64_t _currentSeq;
	uint64_t _inSeq;

	int _socketPort;
	
	// The actual receive queue of the socket.
	std::deque<Packet> _recvQueue;

	// Socket options.
	bool _passCreds;
};

struct Group {
	friend struct OpenFile;

	// Sends a copy of the given message to this group.
	void carbonCopy(const Packet &packet);

private:
	std::vector<OpenFile *> _subscriptions;
};

// ----------------------------------------------------------------------------
// OpenFile implementation.
// ----------------------------------------------------------------------------

COFIBER_ROUTINE(expected<size_t>,
OpenFile::sendMsg(Process *process, MsgFlags flags, const void *data, size_t max_length,
		const void *addr_ptr, size_t addr_length,
		std::vector<smarter::shared_ptr<File, FileHandle>> files), ([=] {
	if(logSockets)
		std::cout << "posix: Send to socket \e[1;34m" << structName() << "\e[0m" << std::endl;
	assert(!flags);
	assert(addr_length == sizeof(struct sockaddr_nl));
	assert(files.empty());

	struct sockaddr_nl sa;
	memcpy(&sa, addr_ptr, sizeof(struct sockaddr_nl));

	int grp_idx = 0;
	if(sa.nl_groups) {
		// Linux allows multicast only to a single group at a time.
		grp_idx = __builtin_ffs(sa.nl_groups);
		assert(sa.nl_groups == (1 << (grp_idx - 1)));
	}

	// TODO: Associate port otherwise.
	assert(_socketPort);

	Packet packet;
	packet.senderPid = process->pid();
	packet.senderPort = _socketPort;
	packet.group = grp_idx;
	packet.buffer.resize(max_length);
	memcpy(packet.buffer.data(), data, max_length);

	// Carbon-copy to the message to a group.
	if(grp_idx) {
		auto it = globalGroupMap.find({_protocol, grp_idx});
		assert(it != globalGroupMap.end());
		auto group = it->second.get();
		group->carbonCopy(packet);
	}

	// Netlink delivers the message per unicast.
	// This is done even if the target address includes multicast groups.
	if(sa.nl_pid) {
		// Deliver to a user-mode socket.
		auto it = globalPortMap.find(sa.nl_pid);
		assert(it != globalPortMap.end());

		it->second->deliver(std::move(packet));
	}else{
		// TODO: Deliver the message a listener function.
	}

	COFIBER_RETURN(max_length);
}))

COFIBER_ROUTINE(async::result<void>,
OpenFile::bind(Process *, const void *addr_ptr, size_t addr_length), ([=] {
	struct sockaddr_nl sa;
	assert(addr_length <= sizeof(struct sockaddr_nl));
	memcpy(&sa, addr_ptr, addr_length);

	assert(!sa.nl_pid);
	_associatePort();

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

COFIBER_ROUTINE(async::result<size_t>,
OpenFile::sockname(void *addr_ptr, size_t max_addr_length), ([=] {
	assert(_socketPort);

	// TODO: Fill in nl_groups.
	struct sockaddr_nl sa;
	memset(&sa, 0, sizeof(struct sockaddr_nl));
	sa.nl_family = AF_NETLINK;
	sa.nl_pid = _socketPort;
	memcpy(addr_ptr, &sa, std::min(sizeof(struct sockaddr_nl), max_addr_length));
	
	COFIBER_RETURN(sizeof(struct sockaddr_nl));
}))

// ----------------------------------------------------------------------------
// Group implementation.
// ----------------------------------------------------------------------------

void Group::carbonCopy(const Packet &packet) {
	for(auto socket : _subscriptions)
		socket->deliver(packet);
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
	Packet packet;
	packet.senderPid = 0;
	packet.senderPort = 0;
	packet.group = grp_idx;
	packet.buffer.resize(buffer.size());
	memcpy(packet.buffer.data(), buffer.data(), buffer.size());

	auto it = globalGroupMap.find({proto_idx, grp_idx});
	assert(it != globalGroupMap.end());
	auto group = it->second.get();
	group->carbonCopy(packet);
}

smarter::shared_ptr<File, FileHandle> createSocketFile(int protocol) {
	auto file = smarter::make_shared<OpenFile>(protocol);
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

} // namespace nl_socket

