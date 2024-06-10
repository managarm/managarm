#pragma once

#include <async/result.hpp>
#include <async/recurring-event.hpp>
#include <protocols/fs/server.hpp>
#include <netserver/nic.hpp>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "core/netlink.hpp"
#include "ip/ip4.hpp"
#include "ip/arp.hpp"

#include <deque>
#include <vector>

namespace nl {

class NetlinkSocket final : core::netlink::NetlinkFile {
public:
	NetlinkSocket(int flags);

	static async::result<protocols::fs::RecvResult> recvMsg(void *obj,
			const char *creds, uint32_t flags, void *data,
			size_t len, void *addr_buf, size_t addr_size, size_t max_ctrl_len);

	static async::result<frg::expected<protocols::fs::Error, size_t>> sendMsg(void *obj,
			const char *creds, uint32_t flags, void *data, size_t len,
			void *addr_ptr, size_t addr_size, std::vector<uint32_t> fds, struct ucred ucreds);


	static async::result<protocols::fs::Error> bind(void *obj, const char *creds,
			const void *addr_ptr, size_t addr_length) {
		(void) obj;
		(void) creds;
		(void) addr_ptr;
		(void) addr_length;
		co_return protocols::fs::Error::none;
	}

	static async::result<void> setOption(void *, int option, int value);

	static async::result<size_t> sockname(void *, void *, size_t);

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
	pollWait(void *object, uint64_t sequence, int mask, async::cancellation_token cancellation);

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
	pollStatus(void *object);

	constexpr static protocols::fs::FileOperations ops {
		.setOption = &setOption,
		.pollWait = &pollWait,
		.pollStatus = &pollStatus,
		.bind = &bind,
		.sockname = &sockname,
		.recvMsg = &recvMsg,
		.sendMsg = &sendMsg,
	};

	void deliver(core::netlink::Packet packet) override;

private:
	void getRoute(struct nlmsghdr *hdr);
	void newRoute(struct nlmsghdr *hdr);

	void getLink(struct nlmsghdr *hdr);

	void newAddr(struct nlmsghdr *hdr);
	void getAddr(struct nlmsghdr *hdr);
	void deleteAddr(struct nlmsghdr *hdr);

	void getNeighbor(struct nlmsghdr *hdr);

	void sendLinkPacket(std::shared_ptr<nic::Link> nic, void *h);
	void sendAddrPacket(const struct nlmsghdr *hdr, const struct ifaddrmsg *msg, std::shared_ptr<nic::Link>);
	void sendRoutePacket(const struct nlmsghdr *hdr, Ip4Router::Route &route);
	void sendNeighPacket(const struct nlmsghdr *hdr, uint32_t addr, Neighbours::Entry &entry);

	int flags;

	// Status management for poll()
	async::recurring_event _statusBell;
	bool _isClosed = false;
	uint64_t _currentSeq;
	uint64_t _inSeq;
	bool _passCreds = false;

	std::deque<core::netlink::Packet> _recvQueue;
};

} // namespace nl
