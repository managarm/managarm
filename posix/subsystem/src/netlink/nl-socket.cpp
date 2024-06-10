#include <core/bpf.hpp>
#include <linux/netlink.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <linux/filter.h>
#include <iostream>
#include <map>
#include <format>

#include <async/recurring-event.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/common.hpp>
#include "nl-socket.hpp"
#include "uevent.hpp"
#include "../process.hpp"

namespace netlink::nl_socket {

struct OpenFile;
struct Group;

constexpr bool logSockets = false;

std::map<int, const ops *> globalProtocolOpsMap;

/* (protocol, groupid) -> std::unique_ptr<Group> */
std::map<std::pair<int, uint8_t>, std::unique_ptr<Group>> globalGroupMap;

uint32_t nextPort = UINT32_MAX;
std::map<uint32_t, OpenFile *> globalPortMap;

// ----------------------------------------------------------------------------
// OpenFile implementation.
// ----------------------------------------------------------------------------

OpenFile::OpenFile(int protocol, bool nonBlock)
		: File{StructName::get("nl-socket"), nullptr,
		SpecialLink::makeSpecialLink(VfsType::socket, 0777), File::defaultPipeLikeSeek},
		_protocol{protocol}, ops_(globalProtocolOpsMap.at(protocol)), _currentSeq{1},
		_inSeq{0}, _socketPort{0}, _passCreds{false}, nonBlock_{nonBlock} { }

void OpenFile::deliver(core::netlink::Packet packet) {
	if(filter_) {
		Bpf bpf{filter_.value()};
		size_t accept_bytes = bpf.run(arch::dma_buffer_view{nullptr, packet.buffer.data(), packet.buffer.size()});

		if(!accept_bytes)
			return;

		if(accept_bytes < packet.buffer.size())
			packet.buffer.resize(accept_bytes);
	}

	_recvQueue.push_back(std::move(packet));
	_inSeq = ++_currentSeq;
	_statusBell.raise();
}

async::result<frg::expected<Error, size_t>>
OpenFile::readSome(Process *, void *data, size_t max_length) {
	if(logSockets)
		std::cout << "posix: Read from socket " << this << std::endl;

	if(_recvQueue.empty() && nonBlock_) {
		if(logSockets)
			std::cout << "posix: netlink socket would block" << std::endl;
		co_return Error::wouldBlock;
	}

	while(_recvQueue.empty())
		co_await _statusBell.async_wait();

	// TODO: Truncate packets (for SOCK_DGRAM) here.
	auto packet = &_recvQueue.front();
	auto size = packet->buffer.size();
	assert(max_length >= size);
	memcpy(data, packet->buffer.data(), size);
	_recvQueue.pop_front();
	co_return size;
}

async::result<frg::expected<Error, size_t>>
OpenFile::writeAll(Process *, const void *data, size_t length) {
	(void) data;
	(void) length;

	throw std::runtime_error("posix: Fix netlink send()");
/*
	if(logSockets)
		std::cout << "posix: Write to socket " << this << std::endl;

	Packet packet;
	packet.buffer.resize(length);
	memcpy(packet.buffer.data(), data, length);

	_remote->deliver(std::move(packet));
*/

	co_return {};
}

async::result<protocols::fs::RecvResult>
OpenFile::recvMsg(Process *, uint32_t flags, void *data, size_t max_length,
		void *addr_ptr, size_t max_addr_length, size_t max_ctrl_length) {
	(void) max_addr_length;

	using namespace protocols::fs;
	if(logSockets)
		std::cout << "posix: Recv from socket \e[1;34m" << structName() << "\e[0m" << std::endl;
	if(flags & ~(MSG_DONTWAIT | MSG_CMSG_CLOEXEC | MSG_PEEK | MSG_TRUNC)) {
		std::cout << std::format("posix: Unsupported flags 0x{:x} in recvMsg", flags) << std::endl;
	}

	if(_recvQueue.empty() && ((flags & MSG_DONTWAIT) || nonBlock_)) {
		if(logSockets)
			std::cout << "posix: netlink socket would block" << std::endl;
		co_return RecvResult { protocols::fs::Error::wouldBlock };
	}

	while(_recvQueue.empty())
		co_await _statusBell.async_wait();

	// TODO: Truncate packets (for SOCK_DGRAM) here.
	auto packet = &_recvQueue.front();

	auto size = packet->buffer.size();
	auto truncated_size = std::min(size, max_length);

	auto chunk = std::min(packet->buffer.size() - packet->offset, max_length);
	memcpy(data, packet->buffer.data() + packet->offset, chunk);
	if(!(flags & MSG_PEEK)) {
		packet->offset += chunk;
	}

	if(addr_ptr) {
		struct sockaddr_nl sa;
		memset(&sa, 0, sizeof(struct sockaddr_nl));
		sa.nl_family = AF_NETLINK;
		sa.nl_pid = packet->senderPort;
		sa.nl_groups = packet->group ? (1 << (packet->group - 1)) : 0;
		memcpy(addr_ptr, &sa, sizeof(struct sockaddr_nl));
	}

	protocols::fs::CtrlBuilder ctrl{max_ctrl_length};

	if(_passCreds) {
		struct ucred creds;
		memset(&creds, 0, sizeof(struct ucred));
		creds.pid = packet->senderPid;

		if(!ctrl.message(SOL_SOCKET, SCM_CREDENTIALS, sizeof(struct ucred)))
			throw std::runtime_error("posix: Implement CMSG truncation");
		ctrl.write<struct ucred>(creds);
	}

	if(!(flags & MSG_PEEK)) {
		_recvQueue.pop_front();
	}

	uint32_t reply_flags = 0;

	if(truncated_size < size) {
		reply_flags |= MSG_TRUNC;
	}

	co_return RecvData{ctrl.buffer(), (flags & MSG_TRUNC) ? size : truncated_size,
		sizeof(struct sockaddr_nl), reply_flags};
}

async::result<frg::expected<protocols::fs::Error, size_t>>
OpenFile::sendMsg(Process *process, uint32_t flags, const void *data, size_t max_length,
		const void *addr_ptr, size_t addr_length,
		std::vector<smarter::shared_ptr<File, FileHandle>> files, struct ucred) {
	if(logSockets)
		std::cout << "posix: Send to socket \e[1;34m" << structName() << "\e[0m" << std::endl;
	assert(!flags);
	assert(files.empty());

	struct sockaddr_nl sa;
	if(addr_length >= sizeof(struct sockaddr_nl) && addr_ptr) {
		memcpy(&sa, addr_ptr, sizeof(struct sockaddr_nl));
	} else {
		memset(&sa, 0, sizeof(sa));
		sa.nl_family = AF_NETLINK;
	}

	size_t grp_idx = 0;
	if(sa.nl_groups) {
		// Linux allows multicast only to a single group at a time.
		grp_idx = __builtin_ffs(sa.nl_groups);
	}

	// TODO: Associate port otherwise.
	assert(_socketPort);

	core::netlink::Packet packet;
	packet.senderPid = process->pid();
	packet.senderPort = _socketPort;
	packet.group = grp_idx;
	packet.buffer.resize(max_length);
	memcpy(packet.buffer.data(), data, max_length);

	if(ops_ && ops_->sendMsg)
		ops_->sendMsg(packet, &sa);
	else
		co_return protocols::fs::Error::illegalOperationTarget;

	co_return max_length;
}

async::result<frg::expected<Error, PollWaitResult>>
OpenFile::pollWait(Process *, uint64_t past_seq, int mask,
		async::cancellation_token cancellation) {
	(void)mask; // TODO: utilize mask.
	if(_isClosed)
		co_return Error::fileClosed;

	assert(past_seq <= _currentSeq);
	while(past_seq == _currentSeq && !cancellation.is_cancellation_requested())
		co_await _statusBell.async_wait(cancellation);

	if(_isClosed)
		co_return Error::fileClosed;

	// For now making sockets always writable is sufficient.
	int edges = EPOLLOUT;
	if(_inSeq > past_seq)
		edges |= EPOLLIN;

//		std::cout << "posix: pollWait(" << past_seq << ") on \e[1;34m" << structName() << "\e[0m"
//				<< " returns (" << _currentSeq
//				<< ", " << edges << ")" << std::endl;

	co_return PollWaitResult(_currentSeq, edges);
}

async::result<frg::expected<Error, PollStatusResult>> OpenFile::pollStatus(Process *) {
	int events = EPOLLOUT;
	if(!_recvQueue.empty())
		events |= EPOLLIN;

	co_return PollStatusResult(_currentSeq, events);
}

async::result<protocols::fs::Error> OpenFile::bind(Process *,
		const void *addr_ptr, size_t addr_length) {
	if(addr_length < sizeof(struct sockaddr_nl))
		co_return protocols::fs::Error::illegalArguments;

	struct sockaddr_nl sa;
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
	co_return protocols::fs::Error::none;
}

async::result<size_t> OpenFile::sockname(void *addr_ptr, size_t max_addr_length) {
	assert(_socketPort);

	// TODO: Fill in nl_groups.
	struct sockaddr_nl sa;
	memset(&sa, 0, sizeof(struct sockaddr_nl));
	sa.nl_family = AF_NETLINK;
	sa.nl_pid = _socketPort;
	memcpy(addr_ptr, &sa, std::min(sizeof(struct sockaddr_nl), max_addr_length));

	co_return sizeof(struct sockaddr_nl);
}

async::result<frg::expected<protocols::fs::Error>> OpenFile::setSocketOption(int layer, int number,
		std::vector<char> optbuf) {
	if(layer == SOL_SOCKET && number == SO_ATTACH_FILTER) {
		assert(optbuf.size() % sizeof(struct sock_filter) == 0);

		Bpf bpf{optbuf};
		if(!bpf.validate())
			co_return protocols::fs::Error::illegalArguments;

		filter_ = optbuf;
	} else {
		printf("netserver: unhandled setsockopt layer %d number %d\n", layer, number);
		co_return protocols::fs::Error::invalidProtocolOption;
	}

	co_return {};
}

async::result<void> OpenFile::setFileFlags(int flags) {
	if (flags & ~O_NONBLOCK) {
		std::cout << "posix: setFileFlags on netlink socket \e[1;34m" << structName() << "\e[0m called with unknown flags" << std::endl;
		co_return;
	}
	if (flags & O_NONBLOCK)
		nonBlock_ = true;
	else
		nonBlock_ = false;
	co_return;
}

async::result<int> OpenFile::getFileFlags() {
	if(nonBlock_)
		co_return O_NONBLOCK;
	co_return 0;
}

void OpenFile::_associatePort() {
	assert(!_socketPort);
	_socketPort = nextPort--;
	auto res = globalPortMap.insert({_socketPort, this});
	assert(res.second);
}

// ----------------------------------------------------------------------------
// Group implementation.
// ----------------------------------------------------------------------------

void Group::carbonCopy(const core::netlink::Packet &packet) {
	for(auto socket : _subscriptions)
		socket->deliver(packet);
}

// ----------------------------------------------------------------------------
// Free functions.
// ----------------------------------------------------------------------------

void setupProtocols() {
	configure(NETLINK_KOBJECT_UEVENT, 32, &netlink::uevent::ops);
	configure(NETLINK_ROUTE, 32, nullptr);
}

void configure(int protocol, size_t num_groups, const ops *ops) {
	assert(num_groups <= 32);

	globalProtocolOpsMap.insert({protocol, ops});

	for(size_t i = 0; i < num_groups; i++) {
		std::pair<int, int> idx{protocol, i + 1};
		auto res = globalGroupMap.insert(std::make_pair(idx, std::make_unique<Group>()));
		assert(res.second);
	}
}

void broadcast(int proto_idx, uint32_t grp_idx, std::string buffer) {
	core::netlink::Packet packet{
		.group = grp_idx,
	};
	packet.buffer.resize(buffer.size());
	memcpy(packet.buffer.data(), buffer.data(), buffer.size());

	auto it = globalGroupMap.find({proto_idx, grp_idx});
	assert(it != globalGroupMap.end());
	auto group = it->second.get();
	group->carbonCopy(packet);
}

bool protocol_supported(int protocol) {
	return globalProtocolOpsMap.contains(protocol);
}

smarter::shared_ptr<File, FileHandle> createSocketFile(int protocol, bool nonBlock) {
	auto file = smarter::make_shared<OpenFile>(protocol, nonBlock);
	file->setupWeakFile(file);
	OpenFile::serve(file);
	return File::constructHandle(std::move(file));
}

} // namespace netlink::nl_socket

