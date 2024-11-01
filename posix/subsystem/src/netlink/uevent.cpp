#include "uevent.hpp"

namespace netlink {

async::result<protocols::fs::Error>
uevent::sendMsg(nl_socket::OpenFile *, core::netlink::Packet packet, struct sockaddr_nl *sa) {
	// Carbon-copy to the message to a group.
	if (packet.group) {
		auto it = nl_socket::globalGroupMap.find({NETLINK_KOBJECT_UEVENT, packet.group});
		assert(it != nl_socket::globalGroupMap.end());
		auto group = it->second.get();
		group->carbonCopy(packet);
	}

	// Netlink delivers the message per unicast.
	// This is done even if the target address includes multicast groups.
	if (sa->nl_pid) {
		// Deliver to a user-mode socket.
		auto it = nl_socket::globalPortMap.find(sa->nl_pid);
		assert(it != nl_socket::globalPortMap.end());

		it->second->deliver(std::move(packet));
	} else {
		// TODO: Deliver the message a listener function.
	}

	co_return protocols::fs::Error::none;
}

} // namespace netlink
