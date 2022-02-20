#include "netlink.hpp"
#include "ip/ip4.hpp"

#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

namespace {
	constexpr bool logSocket = false;
}

namespace nl {

NetlinkSocket::NetlinkSocket(int flags)
: flags(flags)
{ }

async::result<protocols::fs::RecvResult> NetlinkSocket::recvMsg(void *obj,
		const char *creds, uint32_t flags, void *data,
		size_t len, void *addr_buf, size_t addr_size, size_t max_ctrl_len) {
	auto *self = static_cast<NetlinkSocket*>(obj);
	if (logSocket)
		std::cout << "netserver: Recv from netlink socket" << std::endl;

	if (addr_size >= sizeof(struct sockaddr_nl))
		co_return protocols::fs::Error::illegalArguments;

	while(self->_recvQueue.empty())
		co_await self->_statusBell.async_wait();

	auto packet = &self->_recvQueue.front();

	// TODO: Do truncation.
	const auto size = packet->buffer.size();
	assert(size < len);
	memcpy(data, packet->buffer.data(), size);

	if (addr_size >= sizeof(struct sockaddr_nl)) {
		struct sockaddr_nl sa;
		memset(&sa, 0, sizeof(struct sockaddr_nl));
		sa.nl_family = AF_NETLINK;
		sa.nl_pid = 0;
		sa.nl_groups = packet->group ? (1 << (packet->group - 1)) : 0;
		memcpy(addr_buf, &sa, sizeof(struct sockaddr_nl));
	}

	protocols::fs::CtrlBuilder ctrl{max_ctrl_len};

	self->_recvQueue.pop_front();
	co_return protocols::fs::RecvResult {protocols::fs::RecvData{size, sizeof(struct sockaddr_nl), ctrl.buffer()}};
}

async::result<frg::expected<protocols::fs::Error, size_t>> NetlinkSocket::sendMsg(void *obj,
			const char *creds, uint32_t flags, void *data, size_t len,
			void *addr_ptr, size_t addr_size, std::vector<uint32_t> fds) {
	if (logSocket)
		std::cout << "netserver: sendMsg on netlink socket!" << std::endl;
	const auto orig_len = len;
	auto self = static_cast<NetlinkSocket*>(obj);
	assert(!flags);
	assert(fds.empty());

	auto push_buffer = [] (std::vector<char> &buffer, auto to_push, int &offset) {
		const size_t push_size = sizeof(to_push);
		buffer.resize(offset + push_size);
		memcpy(buffer.data() + offset, &to_push, push_size);
		offset += push_size;
	};

	auto hdr = static_cast<struct nlmsghdr *>(data);
	for (; NLMSG_OK(hdr, len); hdr = NLMSG_NEXT(hdr, len)) {
		if (hdr->nlmsg_type == NLMSG_DONE)
			co_return orig_len;

		// TODO: maybe send an error packet back instead of erroring here?
		if (hdr->nlmsg_type == NLMSG_ERROR)
			co_return protocols::fs::Error::illegalArguments;

		if (hdr->nlmsg_type == RTM_GETROUTE) {
			assert(hdr->nlmsg_flags == (NLM_F_REQUEST | NLM_F_DUMP));
			auto payload = static_cast<struct rtgenmsg *>(NLMSG_DATA(hdr));
			assert(payload->rtgen_family == AF_UNSPEC);

			// Loop over all ipv4 and ipv6 routes, and return them.

			int push_offset = 0;
			Packet packet;
			packet.group = 0;

			auto ipv4_router = ip4Router();
			struct nlmsghdr send_hdr;
			send_hdr.nlmsg_type = RTM_NEWROUTE;
			// TODO: also return ipv6 routes.
			send_hdr.nlmsg_len = NLMSG_LENGTH(3 * ipv4_router.getRoutes().size() *
					(sizeof(struct rtattr) + 4) + sizeof(struct rtmsg));
			send_hdr.nlmsg_flags = 0;
			send_hdr.nlmsg_seq = hdr->nlmsg_seq;
			send_hdr.nlmsg_pid = 0; // TODO: this should have some value but I'm not sure which.

			push_buffer(packet.buffer, send_hdr, push_offset);

			struct rtmsg route_msg;
			route_msg.rtm_family = AF_INET;
			route_msg.rtm_dst_len = 32;
			route_msg.rtm_src_len = 0;
			route_msg.rtm_tos = 0;
			route_msg.rtm_table = RT_TABLE_UNSPEC;
			route_msg.rtm_protocol = RTPROT_UNSPEC;
			route_msg.rtm_scope = RT_SCOPE_UNIVERSE;
			route_msg.rtm_type = RTN_LOCAL;
			route_msg.rtm_flags = 0;

			push_buffer(packet.buffer, route_msg, push_offset);

			for (auto route : ipv4_router.getRoutes()) {
				struct rtattr gate_addr;
				gate_addr.rta_len = 8;
				gate_addr.rta_type = RTA_GATEWAY;
				push_buffer(packet.buffer, gate_addr, push_offset);
				push_buffer(packet.buffer, htonl(route.gateway), push_offset);

				struct rtattr src_addr;
				src_addr.rta_len = 8;
				src_addr.rta_type = RTA_SRC;
				push_buffer(packet.buffer, src_addr, push_offset);
				push_buffer(packet.buffer, htonl(route.source), push_offset);

				struct rtattr dst_addr;
				dst_addr.rta_len = 8;
				dst_addr.rta_type = RTA_DST;
				push_buffer(packet.buffer, dst_addr, push_offset);
				push_buffer(packet.buffer, htonl(route.network.ip), push_offset);
			}

			self->_recvQueue.push_back(packet);
		} else {
			// TODO: Handle more message types (such as updating routes).
			co_return protocols::fs::Error::illegalArguments;
		}
	}

	co_return orig_len;
}

} // namespace nl
