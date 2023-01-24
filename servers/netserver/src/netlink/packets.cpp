#include "netlink.hpp"
#include "netserver/nic.hpp"
#include "packets.hpp"

#include <abi-bits/socket.h>
#include <arpa/inet.h>
#include <linux/if_arp.h>
#include <linux/neighbour.h>
#include <linux/rtnetlink.h>
#include <net/ethernet.h>

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
}

} // namespace nl
