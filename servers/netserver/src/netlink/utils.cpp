#include "netlink.hpp"
#include "src/netlink/packets.hpp"

namespace nl {

void NetlinkSocket::sendAck(struct nlmsghdr *hdr) {
	NetlinkBuilder b;

	b.header(NLMSG_ERROR, NLM_F_CAPPED, hdr->nlmsg_seq, 0);
	b.message<struct nlmsgerr>({
		.error = 0,
		.msg = *hdr,
	});

	_recvQueue.push_back(b.packet());
}

void NetlinkSocket::sendDone(struct nlmsghdr *hdr) {
	NetlinkBuilder b;

	b.header(NLMSG_DONE, 0, hdr->nlmsg_seq, 0);
	b.message<uint32_t>(0);

	_recvQueue.push_back(b.packet());
}

void NetlinkSocket::sendError(struct nlmsghdr *hdr, int err) {
	NetlinkBuilder b;

	b.header(NLMSG_ERROR, 0, hdr->nlmsg_seq, 0);

	b.message<struct nlmsgerr>({
		.error = -err,
		.msg = *hdr,
	});

	_recvQueue.push_back(b.packet());
}

} // namespace nl
