#pragma once

#include "nl-socket.hpp"

namespace netlink {

struct uevent {
	static void sendMsg(core::netlink::Packet &packet, struct sockaddr_nl *sa);

	constexpr static struct nl_socket::ops ops{
		.sendMsg = sendMsg,
	};
};

} // namespace netlink
