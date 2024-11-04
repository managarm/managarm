#pragma once

#include "nl-socket.hpp"

namespace netlink {

struct nlctrl {
	static async::result<protocols::fs::Error>
	sendMsg(nl_socket::OpenFile *f, core::netlink::Packet packet, struct sockaddr_nl *sa);

	constexpr static struct nl_socket::ops ops{
	    .sendMsg = sendMsg,
	};
};

} // namespace netlink
