#include "netlink.hpp"
#include "src/ip/arp.hpp"
#include "src/netlink/packets.hpp"

#include <arpa/inet.h>
#include <linux/neighbour.h>

namespace nl {

void NetlinkSocket::getLink(struct nlmsghdr *hdr) {
	const struct ifinfomsg *msg;

	if(auto opt = netlinkMessage<struct ifinfomsg>(hdr, hdr->nlmsg_len))
		msg = *opt;
	else {
		sendError(hdr, EINVAL);
		return;
	}

	std::optional<std::string> if_name = std::nullopt;
	[[maybe_unused]] uint32_t ext_mask = 0;

	if(msg) {
		auto attrs = NetlinkAttr(hdr, nl::packets::ifinfo{});

		if(!attrs.has_value()) {
			sendError(hdr, EINVAL);
			return;
		}

		for(auto attr : *attrs) {
			switch(attr.type()) {
				case IFLA_IFNAME: {
					if(auto opt = attr.str()) {
						if_name = opt;
					} else {
						std::cout << "netlink: string parsing from rtattr failed unexpectedly" << std::endl;
						sendError(hdr, EINVAL);
						return;
					}
					break;
				}
				case IFLA_EXT_MASK: {
					ext_mask = attr.data<uint32_t>().value_or(0);
					break;
				}
				default: {
					std::cout << "netlink: ignoring unknown attr " << attr.type() << std::endl;
					if(attr.type() > RTA_MAX) {
						sendError(hdr, EINVAL);
						return;
					}
					break;
				}
			}
		}
	}

	if(!msg || msg->ifi_index == 0) {
		auto links = nic::Link::getLinks();

		for(auto m = links.begin(); m != links.end(); m++) {
			if(!if_name.has_value() || if_name == m->second->name()) {
				sendLinkPacket(m->second, hdr);
			}
		}
	} else {
		auto nic = nic::Link::byIndex(msg->ifi_index);

		if(!nic) {
			sendError(hdr, ENODEV);
			return;
		}

		if(!if_name.has_value() || if_name == nic->name())
			sendLinkPacket(nic, hdr);
	}

	sendDone(hdr);

	return;
}

void NetlinkSocket::newRoute(struct nlmsghdr *hdr) {
	const struct rtmsg *msg;

	if(auto opt = netlinkMessage<struct rtmsg>(hdr, hdr->nlmsg_len))
		msg = *opt;
	else {
		sendError(hdr, EINVAL);
		return;
	}

	auto attrs = NetlinkAttr(hdr, nl::packets::rt{});

	if(!attrs.has_value()) {
		sendError(hdr, EINVAL);
		return;
	}

	Ip4Router::Route route { { 0, 0 }, {} };
	bool route_changed = false;

	for(auto attr : *attrs) {
		switch(attr.type()) {
			case RTA_DST: {
				uint32_t dst = ntohl(attr.data<uint32_t>().value_or(0));
				uint32_t dst_len = msg->rtm_dst_len;
				route.network.ip = dst;
				route.network.prefix = dst_len;
				route_changed = true;
				break;
			}
			case RTA_GATEWAY: {
				uint32_t gateway = ntohl(attr.data<uint32_t>().value_or(0));
				route.gateway = gateway;
				route_changed = true;
				break;
			}
			case RTA_PREFSRC: {
				uint32_t prefsrc = ntohl(attr.data<uint32_t>().value_or(0));
				route.source = prefsrc;
				route_changed = true;
				break;
			}
			case RTA_OIF: {
				int if_index = attr.data<int>().value_or(0);
				auto nic = nic::Link::byIndex(if_index);
				if(nic) {
					route.link = nic;
					route_changed = true;
				}
				break;
			}
			case RTA_PRIORITY: {
				int metric = attr.data<int>().value_or(0);
				route.metric = metric;
				route_changed = true;
				break;
			}
			default:
				std::cout << "netlink: ignoring unknown attr " << attr.type() << std::endl;
				if(attr.type() > RTA_MAX) {
					sendError(hdr, EINVAL);
					return;
				}
				break;
		}
	}

	if(msg->rtm_protocol)
		route.protocol = msg->rtm_protocol;
	if(msg->rtm_type)
		route.type = msg->rtm_type;
	if(msg->rtm_scope)
		route.scope = msg->rtm_scope;
	if(msg->rtm_flags)
		route.flags = msg->rtm_flags;
	if(msg->rtm_family)
		route.family = msg->rtm_family;

	if(route_changed)
		ip4Router().addRoute(std::move(route));

	if(hdr->nlmsg_flags & NLM_F_ACK)
		sendAck(hdr);
}

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

void NetlinkSocket::newAddr(struct nlmsghdr *hdr) {
	const struct ifaddrmsg *msg;

	if(auto opt = netlinkMessage<struct ifaddrmsg>(hdr, hdr->nlmsg_len))
		msg = *opt;
	else {
		sendError(hdr, EINVAL);
		return;
	}

	auto attrs = NetlinkAttr(hdr, nl::packets::ifaddr{});

	if(!attrs.has_value()) {
		sendError(hdr, EINVAL);
		return;
	}

	uint32_t addr = 0;
	uint8_t prefix = msg->ifa_prefixlen;
	auto nic = nic::Link::byIndex(msg->ifa_index);

	if(!nic) {
		sendError(hdr, ENODEV);
		return;
	}

	for(auto &attr : *attrs) {
		switch(attr.type()) {
			case IFA_ADDRESS: {
				addr = ntohl(attr.data<uint32_t>().value_or(0));
				break;
			}
			case IFA_LOCAL: {
				addr = ntohl(attr.data<uint32_t>().value_or(0));
				break;
			}
			default: {
				std::cout << "netserver: ignoring unknown rtattr type " << attr.type() << " in RTM_NEWADDR request" << std::endl;
				if(attr.type() > RTA_MAX) {
					sendError(hdr, EINVAL);
					return;
				}
				break;
			}
		}
	}

	if(addr)
		ip4().setLink({addr, prefix}, nic);

	if(hdr->nlmsg_flags & NLM_F_ACK)
		sendAck(hdr);

	return;
}

void NetlinkSocket::getAddr(struct nlmsghdr *hdr) {
	const struct ifaddrmsg *msg;

	if(auto opt = netlinkMessage<struct ifaddrmsg>(hdr, hdr->nlmsg_len))
		msg = *opt;
	else {
		sendError(hdr, EINVAL);
		return;
	}

	auto links = nic::Link::getLinks();
	for(auto m = links.begin(); m != links.end(); m++) {
		if(!msg || msg->ifa_index == 0) {
			sendAddrPacket(hdr, msg, m->second);
		} else if(static_cast<uint32_t>(m->second->index()) == msg->ifa_index) {
			sendAddrPacket(hdr, msg, m->second);
			break;
		}
	}

	sendDone(hdr);

	return;
}

void NetlinkSocket::deleteAddr(struct nlmsghdr *hdr) {
	const struct ifaddrmsg *msg;

	if(auto opt = netlinkMessage<struct ifaddrmsg>(hdr, hdr->nlmsg_len))
		msg = *opt;
	else {
		sendError(hdr, EINVAL);
		return;
	}

	auto attrs = NetlinkAttr(hdr, nl::packets::ifaddr{});

	if(!attrs.has_value()) {
		sendError(hdr, EINVAL);
		return;
	}

	uint32_t addr = 0;
	auto nic = nic::Link::byIndex(msg->ifa_index);

	if(!nic) {
		sendError(hdr, ENODEV);
		return;
	}

	for(auto attr : *attrs) {
		switch(attr.type()) {
			case IFA_ADDRESS: {
				addr = ntohl(attr.data<uint32_t>().value_or(0));

				if(addr) {
					auto nic_by_addr = ip4().getLink(addr);

					if(nic_by_addr == nullptr || nic_by_addr->index() != nic->index()) {
						sendError(hdr, EINVAL);
						return;
					}
				}
				break;
			}
			default: {
				std::cout << "netserver: ignoring unknown rtattr type " << attr.type() << " in RTM_DELADDR request" << std::endl;
				if(attr.type() > RTA_MAX) {
					sendError(hdr, EINVAL);
					return;
				}
				break;
			}
		}
	}

	auto cidr = ip4().getCidrByIndex(msg->ifa_index);

	if(cidr)
		ip4().deleteLink(cidr.value());
	else {
		sendError(hdr, EINVAL);
		return;
	}

	if(hdr->nlmsg_flags & NLM_F_ACK)
		sendAck(hdr);

	return;
}

void NetlinkSocket::getNeighbor(struct nlmsghdr *hdr) {
	auto &table = neigh4().getTable();

	for(auto it = table.begin(); it != table.end(); it++) {
		sendNeighPacket(hdr, it->first, it->second);
	}

	sendDone(hdr);

	return;
}

} // namespace nl
