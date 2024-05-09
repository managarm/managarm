#include "netlink.hpp"
#include "netserver/nic.hpp"
#include "packets.hpp"
#include "src/ip/arp.hpp"

#include <abi-bits/socket.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/if_arp.h>
#include <linux/neighbour.h>
#include <linux/rtnetlink.h>
#include <memory>
#include <net/ethernet.h>
#include <net/if.h>

namespace {

uint16_t mapArpStateToNetlink(Neighbours::State state) {
	switch(state) {
		case Neighbours::State::none: return NUD_NONE;
		case Neighbours::State::probe: return NUD_PROBE;
		case Neighbours::State::failed: return NUD_FAILED;
		case Neighbours::State::reachable: return NUD_REACHABLE;
		case Neighbours::State::stale: return NUD_STALE;
	}

	__builtin_unreachable();
}

}

namespace nl {

void NetlinkSocket::sendLinkPacket(std::shared_ptr<nic::Link> nic, void *h) {
	struct nlmsghdr *hdr = reinterpret_cast<struct nlmsghdr *>(h);

	NetlinkBuilder b;

	b.header(RTM_NEWLINK, NLM_F_MULTI, hdr->nlmsg_seq, 0);

	b.message<struct ifinfomsg>({
		.ifi_family = AF_UNSPEC,
		.ifi_type = ARPHRD_ETHER,
		.ifi_index = nic->index(),
		.ifi_flags = IFF_UP | IFF_LOWER_UP | IFF_RUNNING | IFF_MULTICAST | IFF_BROADCAST,
	});

	constexpr struct ether_addr broadcast_addr = { {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF} };

	if(!nic->name().empty())
		b.rtattr_string(IFLA_IFNAME, nic->name());
	if(nic->mtu)
		b.rtattr(IFLA_MTU, nic->mtu);
	b.rtattr(IFLA_TXQLEN, 1000);
	b.rtattr(IFLA_BROADCAST, broadcast_addr);
	//TODO(no92): separate out the concept of permanent MAC addresses from userspace-configurable ones
	b.rtattr(IFLA_ADDRESS, nic->deviceMac());
	b.rtattr(IFLA_PERM_ADDRESS, nic->deviceMac());
	b.rtattr(IFLA_OPERSTATE, (uint8_t) IF_OPER_UP);
	b.rtattr(IFLA_NUM_TX_QUEUES, 1);

	_recvQueue.push_back(b.packet());
	_inSeq = ++_currentSeq;
	_statusBell.raise();
}

void NetlinkSocket::sendAddrPacket(const struct nlmsghdr *hdr, const struct ifaddrmsg *msg, std::shared_ptr<nic::Link> nic) {
	auto addr_check = ip4().getCidrByIndex(nic->index());

	if(!addr_check)
		return;

	auto addr = addr_check.value();

	NetlinkBuilder b;
	b.header(RTM_NEWADDR, NLM_F_MULTI | NLM_F_DUMP_FILTERED, hdr->nlmsg_seq, 0);
	b.message<struct ifaddrmsg>({
		.ifa_family = AF_INET,
		.ifa_prefixlen = addr.prefix,
		.ifa_flags = msg->ifa_flags,
		.ifa_scope = RT_SCOPE_UNIVERSE,
		.ifa_index = static_cast<uint32_t>(nic->index()),
	});

	b.rtattr(IFA_ADDRESS, htonl(addr.ip));
	b.rtattr(IFA_LOCAL, htonl(addr.ip));
	b.rtattr_string(IFA_LABEL, nic->name());

	_recvQueue.push_back(b.packet());
	_inSeq = ++_currentSeq;
	_statusBell.raise();
}

void NetlinkSocket::sendRoutePacket(const struct nlmsghdr *hdr, Ip4Router::Route &route) {
	NetlinkBuilder b;

	b.header(RTM_NEWROUTE, NLM_F_MULTI, hdr->nlmsg_seq, 0);
	b.message<struct rtmsg>({
		.rtm_family = AF_INET,
		.rtm_dst_len = route.network.prefix,
		.rtm_src_len = 0,
		.rtm_tos = 0,
		.rtm_table = RT_TABLE_MAIN,
		.rtm_protocol = route.protocol,
		.rtm_scope = route.scope,
		.rtm_type = route.type,
		.rtm_flags = route.flags,
	});

	b.rtattr(RTA_TABLE, RT_TABLE_MAIN);
	if(route.network.ip)
		b.rtattr(RTA_DST, htonl(route.network.ip));
	if(route.metric)
		b.rtattr(RTA_PRIORITY, route.metric);
	if(route.gateway)
		b.rtattr(RTA_GATEWAY, htonl(route.gateway));
	if(route.source)
		b.rtattr(RTA_PREFSRC, htonl(route.source));
	b.rtattr(RTA_OIF, (route.link.expired()) ? 0 : route.link.lock()->index());

	_recvQueue.push_back(b.packet());
	_inSeq = ++_currentSeq;
	_statusBell.raise();
}

void NetlinkSocket::sendNeighPacket(const struct nlmsghdr *hdr, uint32_t addr, Neighbours::Entry &entry) {
	NetlinkBuilder b;
	int index = 0;

	if(!entry.link.expired()) {
		auto nic = entry.link.lock();

		if(nic)
			index = nic->index();
	}

	b.header(RTM_NEWNEIGH, NLM_F_MULTI | NLM_F_DUMP_FILTERED, hdr->nlmsg_seq, 0);
	b.message<struct ndmsg>({
		.ndm_family = AF_INET,
		.ndm_ifindex = index,
		.ndm_state = mapArpStateToNetlink(entry.state),
		.ndm_type = RTN_UNICAST,
	});

	b.rtattr(NDA_DST, htonl(addr));
	b.rtattr<uint8_t[6]>(NDA_LLADDR, entry.mac.data());

	_recvQueue.push_back(b.packet());
	_inSeq = ++_currentSeq;
	_statusBell.raise();
}

} // namespace nl
