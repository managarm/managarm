#pragma once

#include <async/result.hpp>
#include <async/recurring-event.hpp>
#include <protocols/fs/server.hpp>

#include <deque>
#include <vector>
#include <utility>

namespace nl {
struct Packet {
	int senderPort;
	int group;

	std::vector<char> buffer;
};

class NetlinkSocket {
public:
	NetlinkSocket(int flags);

	static async::result<protocols::fs::RecvResult> recvMsg(void *obj,
			const char *creds, uint32_t flags, void *data,
			size_t len, void *addr_buf, size_t addr_size, size_t max_ctrl_len);

	static async::result<frg::expected<protocols::fs::Error, size_t>> sendMsg(void *obj,
			const char *creds, uint32_t flags, void *data, size_t len,
			void *addr_ptr, size_t addr_size, std::vector<uint32_t> fds);


	static async::result<protocols::fs::Error> bind(void *obj, const char *creds,
			const void *addr_ptr, size_t addr_length) {
		co_return protocols::fs::Error::afNotSupported;
	}

	constexpr static protocols::fs::FileOperations ops {
		.bind = &bind,
		.recvMsg = &recvMsg,
		.sendMsg = &sendMsg,
	};

private:
	int flags;

	// Status management for poll()
	async::recurring_event _statusBell;
	bool _isClosed = false;
	uint64_t _currentSeq;
	uint64_t _inSeq;

	std::deque<nl::Packet> _recvQueue;
};

} // namespace nl
