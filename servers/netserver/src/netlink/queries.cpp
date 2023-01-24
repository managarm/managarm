#include "netlink.hpp"
#include "protocols/fs/common.hpp"

#include <arpa/inet.h>

namespace nl {

void NetlinkSocket::getRoute(struct nlmsghdr *hdr) {
	assert(hdr->nlmsg_flags == (NLM_F_REQUEST | NLM_F_DUMP));

	const struct rtgenmsg *payload;

	if(auto opt = netlinkMessage<struct rtgenmsg>(hdr, hdr->nlmsg_len))
		payload = *opt;
	else {
		sendError(hdr, EINVAL);
		return;
	}

	assert(payload->rtgen_family == AF_UNSPEC || payload->rtgen_family == AF_INET);

	// Loop over all ipv4 and ipv6 routes, and return them.
	// TODO: also return ipv6 routes.
	auto ipv4_router = ip4Router();

	for(auto route : ipv4_router.getRoutes()) {
		sendRoutePacket(hdr, route);
	}

	sendDone(hdr);
}

} // namespace nl
