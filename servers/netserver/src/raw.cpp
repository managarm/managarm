#include <arpa/inet.h>
#include <async/execution.hpp>
#include <core/bpf.hpp>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <sys/epoll.h>

#include "raw.hpp"

Raw &raw() {
	static Raw inst;
	return inst;
}

managarm::fs::Errors Raw::serveSocket(helix::UniqueLane lane, int type, int proto, int flags) {
	assert(proto == htons(ETH_P_ALL));
	auto raw_socket = smarter::make_shared<RawSocket>(this, flags);
	raw_socket->holder_ = raw_socket;
	sockets_.push_back(raw_socket);
	async::detach(servePassthrough(std::move(lane), raw_socket, &RawSocket::ops));

	return managarm::fs::Errors::SUCCESS;
}

void Raw::feedPacket(arch::dma_buffer_view frame) {
	for (auto s = sockets_.begin(); s != sockets_.end(); s++) {
		size_t accept_bytes = SIZE_MAX;

		if ((*s)->filter_) {
			Bpf bpf{(*s)->filter_.value()};
			accept_bytes = bpf.run(frame);

			if (!accept_bytes)
				continue;
		}

		RawSocket::PacketInfo info{
		    frame.size(), frame.subview(0, std::min(frame.size(), accept_bytes))
		};

		(*s)->queue_.emplace(info);
		(*s)->_inSeq = ++(*s)->_currentSeq;
		(*s)->_statusBell.raise();
	}
}

async::result<protocols::fs::Error>
RawSocket::bind(void *obj, const char *creds, const void *addr_ptr, size_t addr_size) {
	if (!addr_ptr || addr_size < sizeof(sockaddr))
		co_return protocols::fs::Error::illegalArguments;

	auto self = static_cast<RawSocket *>(obj);
	self->parent->binds_.push_back(self->holder_.lock());

	auto sa = reinterpret_cast<const sockaddr *>(addr_ptr);
	if (sa->sa_family != PF_PACKET && addr_size < sizeof(sockaddr_ll))
		co_return protocols::fs::Error::illegalArguments;
	auto sa_ll = reinterpret_cast<const sockaddr_ll *>(addr_ptr);

	assert(sa_ll->sll_protocol == htons(ETH_P_ALL));
	if (sa_ll->sll_ifindex == 0) {
		self->link = nic::Link::getLinks().begin()->second;
	} else {
		self->link = nic::Link::byIndex(sa_ll->sll_ifindex);
	}

	if (!self->link)
		co_return protocols::fs::Error::noBackingDevice;

	co_return protocols::fs::Error::none;
}

async::result<frg::expected<protocols::fs::Error, size_t>>
RawSocket::write(void *obj, const char *credentials, const void *buffer, size_t length) {
	auto self = static_cast<RawSocket *>(obj);
	assert(self->link);

	auto buf = self->link->allocateFrame(length);
	arch::dma_buffer_view view{buf.frame};
	memcpy(view.data(), buffer, length);
	co_await self->link->send(view);

	co_return length;
}

async::result<protocols::fs::RecvResult> RawSocket::recvmsg(
    void *obj,
    const char *creds,
    uint32_t flags,
    void *data,
    size_t len,
    void *addr_buf,
    size_t addr_size,
    size_t max_ctrl_len
) {
	(void)creds;
	(void)flags;
	(void)addr_buf;
	(void)addr_size;

	auto self = static_cast<RawSocket *>(obj);

	auto element = co_await self->queue_.async_get();
	assert(element);

	size_t data_len = std::min(len, element->view.size());
	memcpy(data, element->view.byte_data(), data_len);

	protocols::fs::CtrlBuilder ctrl{max_ctrl_len};

	if (self->packetAuxData_) {
		ctrl.message(SOL_PACKET, PACKET_AUXDATA, sizeof(struct tpacket_auxdata));
		ctrl.write<struct tpacket_auxdata>({
		    .tp_status = (TP_STATUS_USER | TP_STATUS_CSUM_VALID),
		    .tp_len = static_cast<uint32_t>(element->len),
		    .tp_snaplen = static_cast<uint32_t>(element->view.size()),
		});
	}

	co_return protocols::fs::RecvData{ctrl.buffer(), data_len, 0, 0};
}

async::result<frg::expected<protocols::fs::Error>>
RawSocket::setSocketOption(void *obj, int layer, int number, std::vector<char> optbuf) {
	auto self = static_cast<RawSocket *>(obj);

	if (layer == SOL_SOCKET && number == SO_ATTACH_FILTER) {
		assert(optbuf.size() % sizeof(struct sock_filter) == 0);

		if (self->filterLocked_)
			co_return protocols::fs::Error::insufficientPermissions;

		Bpf bpf{optbuf};
		if (!bpf.validate())
			co_return protocols::fs::Error::illegalArguments;

		self->filter_ = optbuf;
	} else if (layer == SOL_SOCKET && number == SO_DETACH_FILTER) {
		if (self->filterLocked_)
			co_return protocols::fs::Error::insufficientPermissions;

		self->filter_ = std::nullopt;
	} else if (layer == SOL_SOCKET && number == SO_LOCK_FILTER) {
		auto opt = *reinterpret_cast<int *>(optbuf.data());
		if (!opt && self->filterLocked_)
			co_return protocols::fs::Error::insufficientPermissions;
		else
			self->filterLocked_ = true;
	} else if (layer == SOL_PACKET && number == PACKET_AUXDATA) {
		auto opt = *reinterpret_cast<int *>(optbuf.data());
		self->packetAuxData_ = (opt != 0);
	} else {
		printf("netserver: unhandled setsockopt layer %d number %d\n", layer, number);
		co_return protocols::fs::Error::invalidProtocolOption;
	}

	co_return {};
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
RawSocket::pollWait(
    void *obj, uint64_t past_seq, int mask, async::cancellation_token cancellation
) {
	auto self = static_cast<RawSocket *>(obj);
	(void)mask; // TODO: utilize mask.

	assert(past_seq <= self->_currentSeq);
	while (past_seq == self->_currentSeq && !cancellation.is_cancellation_requested())
		co_await self->_statusBell.async_wait(cancellation);

	// For now making sockets always writable is sufficient.
	int edges = EPOLLOUT;
	if (self->_inSeq > past_seq)
		edges |= EPOLLIN;

	co_return protocols::fs::PollWaitResult(self->_currentSeq, edges);
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
RawSocket::pollStatus(void *obj) {
	auto self = static_cast<RawSocket *>(obj);
	int events = EPOLLOUT;
	if (!self->queue_.empty())
		events |= EPOLLIN;

	co_return protocols::fs::PollStatusResult(self->_currentSeq, events);
}
