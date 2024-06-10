#pragma once

#include <async/recurring-event.hpp>
#include <linux/netlink.h>
#include <map>

#include "core/netlink.hpp"
#include "../file.hpp"

namespace netlink::nl_socket {

void setupProtocols();

struct Group;
struct OpenFile;

struct ops {
	async::result<protocols::fs::Error> (*sendMsg)(nl_socket::OpenFile *f, core::netlink::Packet packet, struct sockaddr_nl *sa);
};

extern std::map<int, const ops *> globalProtocolOpsMap;
extern std::map<std::pair<int, uint8_t>, std::unique_ptr<Group>> globalGroupMap;
extern std::map<uint32_t, OpenFile *> globalPortMap;

struct OpenFile : File, core::netlink::NetlinkFile {
public:
	static void serve(smarter::shared_ptr<OpenFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &File::fileOperations, file->_cancelServe));
	}

	OpenFile(int protocol, bool nonBlock = false);

	void deliver(core::netlink::Packet packet) override;

	void handleClose() override {
		_isClosed = true;
		_statusBell.raise();
		_cancelServe.cancel();
	}

	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t max_length) override;

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override;

	async::result<protocols::fs::RecvResult>
	recvMsg(Process *, uint32_t flags, void *data, size_t max_length,
			void *addr_ptr, size_t max_addr_length, size_t max_ctrl_length) override;

	async::result<frg::expected<protocols::fs::Error, size_t>>
	sendMsg(Process *process, uint32_t flags,
			const void *data, size_t max_length,
			const void *addr_ptr, size_t addr_length,
			std::vector<smarter::shared_ptr<File, FileHandle>> files, struct ucred ucreds) override;

	async::result<void> setOption(int option, int value) override {
		assert(option == SO_PASSCRED);
		_passCreds = value;
		co_return;
	};

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t past_seq, int mask,
			async::cancellation_token cancellation) override;

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override;

	async::result<protocols::fs::Error>
	bind(Process *, const void *, size_t) override;

	async::result<size_t> sockname(void *, size_t) override;

	async::result<frg::expected<protocols::fs::Error>>
	setSocketOption(int layer, int number, std::vector<char> optbuf) override;

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	async::result<void> setFileFlags(int flags) override;
	async::result<int> getFileFlags() override;

private:
	void _associatePort();

	int _protocol;
	const ops *ops_;
	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	// Status management for poll().
	async::recurring_event _statusBell;
	bool _isClosed = false;
	uint64_t _currentSeq;
	uint64_t _inSeq;

	uint32_t _socketPort;

	// The actual receive queue of the socket.
	std::deque<core::netlink::Packet> _recvQueue;

	// Socket options.
	bool _passCreds;
	bool nonBlock_;

	// BPF filter
	std::optional<std::vector<char>> filter_ = std::nullopt;
};

struct Group {
	friend struct OpenFile;

	// Sends a copy of the given message to this group.
	void carbonCopy(const core::netlink::Packet &packet);

private:
	std::vector<OpenFile *> _subscriptions;
};

// Configures the given netlink protocol.
// TODO: Let this take a callback that is called on receive?
void configure(int proto_idx, size_t num_groups, const ops *ops);

// Broadcasts a kernel message to the given netlink multicast group.
void broadcast(int proto_idx, uint32_t grp_idx, std::string buffer);

bool protocol_supported(int protocol);

smarter::shared_ptr<File, FileHandle> createSocketFile(int proto_idx, bool nonBlock);

} // namespace netlink::nl_socket

