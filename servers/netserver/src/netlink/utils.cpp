#include "netlink.hpp"
#include "src/netlink/packets.hpp"

namespace nl {

void NetlinkSocket::sendDone(struct nlmsghdr *hdr) {
	NetlinkBuilder b;

	b.header(NLMSG_DONE, 0, hdr->nlmsg_seq, 0);
	b.message<uint32_t>(0);

	_recvQueue.push_back(b.packet());
}

} // namespace nl
