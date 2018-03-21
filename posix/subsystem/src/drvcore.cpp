
#include <linux/netlink.h>

#include "drvcore.hpp"
#include "nl-socket.hpp"

namespace drvcore {

void initialize() {
	nl_socket::configure(NETLINK_KOBJECT_UEVENT, 32);
}

void emitHotplug(std::string buffer) {
	nl_socket::broadcast(NETLINK_KOBJECT_UEVENT, 1, std::move(buffer));
}

} // namespace drvcore

