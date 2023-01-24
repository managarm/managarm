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
