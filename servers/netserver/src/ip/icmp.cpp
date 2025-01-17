#include "icmp.hpp"
#include "ip4.hpp"

#include <arch/bit.hpp>
#include <arpa/inet.h>
#include <async/basic.hpp>
#include <async/queue.hpp>
#include <async/recurring-event.hpp>
#include <async/result.hpp>
#include <core/clock.hpp>
#include <cstring>
#include <format>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <protocols/fs/server.hpp>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>

struct IcmpPacket {
	struct Header {
		uint8_t type;
		uint8_t code;
		uint16_t checksum;
		uint32_t rest_of_header;
	} header;
	static_assert(sizeof(Header) == 8);

	arch::dma_buffer_view payload() const {
		return packet->payload();
	}

	bool parse(smarter::shared_ptr<const Ip4Packet> packet) {
		if (packet->payload().size() < sizeof(header)) {
			return false;
		}

		auto now = clk::getRealtime();
		TIMESPEC_TO_TIMEVAL(&recvTimestamp, &now);

		this->packet = std::move(packet);
		return true;
	}

	smarter::shared_ptr<const Ip4Packet> packet;
	struct timeval recvTimestamp;

	std::weak_ptr<nic::Link> link;
};

using namespace protocols::fs;

struct IcmpSocket {
	IcmpSocket(Icmp *parent) : parent_(parent) {}
	~IcmpSocket();

	static smarter::shared_ptr<IcmpSocket> make_socket(Icmp *parent);

	static async::result<RecvResult> recvmsg(void *obj,
			const char *creds,
			uint32_t flags, void *data, size_t len,
			void *addr_buf, size_t addr_size, size_t max_ctrl_len) {
		using arch::endian, arch::convert_endian;
		(void) creds;
		(void) flags;

		if(flags & ~MSG_DONTWAIT) {
			std::cout << std::format("netserver: unsupported flags {:#x} in ICMP recvmsg", flags) << std::endl;
			co_return Error::illegalArguments;
		}

		auto self = static_cast<IcmpSocket *>(obj);
		if(self->queue_.empty() && flags & MSG_DONTWAIT)
			co_return Error::wouldBlock;

		auto element = co_await self->queue_.async_get();

		auto copySize = std::min(element->payload().size(), len);
		std::memcpy(data, element->payload().data(), copySize);

		sockaddr_in addr {
			.sin_family = AF_INET,
			.sin_addr = { convert_endian<endian::big>(element->packet->header.source) },
		};

		std::memset(addr_buf, 0, addr_size);
		std::memcpy(addr_buf, &addr, std::min(addr_size, sizeof(addr)));

		protocols::fs::CtrlBuilder ctrl{max_ctrl_len};

		if(self->ipPacketInfo_) {
			auto truncated = ctrl.message(IPPROTO_IP, IP_PKTINFO, sizeof(struct in_pktinfo));
			if(!truncated)
				ctrl.write<struct in_pktinfo>({
					.ipi_ifindex = unsigned(element->link.lock()->index()),
					.ipi_spec_dst = { .s_addr = convert_endian<endian::big>(element->packet->header.destination) },
					.ipi_addr = { .s_addr = convert_endian<endian::big>(element->packet->header.source) },
				});
		}

		if(self->timestamp_) {
			auto truncated = ctrl.message(SOL_SOCKET, SCM_TIMESTAMP, sizeof(struct timeval));
			if(!truncated)
				ctrl.write(element->recvTimestamp);
		}

		if(self->ipRecvTtl_) {
			auto truncated = ctrl.message(SOL_IP, IP_TTL, sizeof(int));
			if(!truncated)
				ctrl.write<int>(element->packet->header.ttl);
		}

		if(self->ipRetOpts_) {
			arch::dma_buffer_view options = element->packet->header_view().subview(sizeof(Ip4Packet::Header));

			if(options.size()) {
				auto truncated = ctrl.message(SOL_IP, IP_RETOPTS, options.size());
				if(!truncated)
					ctrl.write_buffer(options);
			}
		}

		co_return RecvData{ctrl.buffer(), copySize, sizeof(addr), 0};
	}

	static async::result<frg::expected<protocols::fs::Error, size_t>> sendmsg(void *,
			const char *creds, uint32_t flags,
			void *data, size_t len,
			void *addr_ptr, size_t addr_size,
			std::vector<uint32_t> fds, struct ucred) {
		using arch::endian;
		(void) creds;
		(void) flags;
		(void) fds;

		if(flags) {
			std::cout << std::format("netserver: unsupported flags {:#x} in ICMP sendmsg", flags) << std::endl;
			co_return Error::illegalArguments;
		}

		IcmpPacket::Header header{};
		memcpy(&header, data, sizeof(header));

		if(header.type != ICMP_ECHO || header.code != 0)
			co_return Error::illegalArguments;

		if(addr_size < sizeof(sockaddr_in))
			co_return Error::illegalArguments;

		sockaddr_in target;
		memcpy(&target, addr_ptr, std::min(addr_size, sizeof(sockaddr_in)));

		auto ti = co_await ip4().targetByRemote(arch::convert_endian<endian::big, endian::native>(target.sin_addr.s_addr));
		if (!ti)
			co_return protocols::fs::Error::netUnreachable;

		auto error = co_await ip4().sendFrame(std::move(*ti),
			data, len,
			static_cast<uint16_t>(IpProto::icmp));

		if (error != protocols::fs::Error::none)
			co_return error;

		co_return len;
	}

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
	pollWait(void *obj, uint64_t past_seq, int mask, async::cancellation_token cancellation) {
		auto self = static_cast<IcmpSocket *>(obj);
		(void)mask; // TODO: utilize mask.

		if(past_seq > self->currentSeq_)
			co_return protocols::fs::Error::illegalArguments;
		while(past_seq == self->currentSeq_ && !cancellation.is_cancellation_requested())
			co_await self->statusBell_.async_wait(cancellation);

		// For now making sockets always writable is sufficient.
		int edges = EPOLLOUT;
		if(self->inSeq_ > past_seq)
			edges |= EPOLLIN;

		co_return protocols::fs::PollWaitResult(self->currentSeq_, edges);
	}

	static async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
	pollStatus(void *obj) {
		auto self = static_cast<IcmpSocket *>(obj);
		int events = EPOLLOUT;
		if(!self->queue_.empty())
			events |= EPOLLIN;

		co_return protocols::fs::PollStatusResult(self->currentSeq_, events);
	}

	static async::result<frg::expected<Error>> setSocketOption(void *obj,
		int layer, int number, std::vector<char> optbuf) {
		auto self = static_cast<IcmpSocket *>(obj);

		if(layer == SOL_IP && number == IP_PKTINFO) {
			if(optbuf.size() != sizeof(int))
				co_return Error::illegalArguments;

			int val = *reinterpret_cast<int *>(optbuf.data());

			self->ipPacketInfo_ = (val != 0);
		} else if(layer == SOL_IP && number == IP_RECVTTL) {
			if(optbuf.size() != sizeof(int))
				co_return Error::illegalArguments;

			int val = *reinterpret_cast<int *>(optbuf.data());

			self->ipRecvTtl_ = (val != 0);
		} else if(layer == SOL_IP && number == IP_RETOPTS) {
			if(optbuf.size() != sizeof(int))
				co_return Error::illegalArguments;

			int val = *reinterpret_cast<int *>(optbuf.data());

			self->ipRetOpts_ = (val != 0);
		} else if(layer == SOL_SOCKET && number == SO_TIMESTAMP) {
			if(optbuf.size() != sizeof(int))
				co_return Error::illegalArguments;

			int val = *reinterpret_cast<int *>(optbuf.data());

			self->timestamp_ = (val != 0);
		} else {
			printf("netserver: unhandled setsockopt layer %d number %d\n", layer, number);
			co_return protocols::fs::Error::invalidProtocolOption;
		}

		co_return {};
	}

	constexpr static FileOperations ops {
		.pollWait = &pollWait,
		.pollStatus = &pollStatus,
		.recvMsg = &recvmsg,
		.sendMsg = &sendmsg,
		.setSocketOption = &setSocketOption,
	};

public:
	async::queue<IcmpPacket, frg::stl_allocator> queue_;
	Icmp *parent_;

	async::recurring_event statusBell_;
	uint64_t currentSeq_;
	uint64_t inSeq_;

	frg::default_list_hook<IcmpSocket> listHook_;

	bool ipPacketInfo_ = false;
	bool ipRecvTtl_ = false;
	bool ipRetOpts_ = false;
	bool timestamp_ = false;
};

frg::intrusive_list<IcmpSocket, frg::locate_member<IcmpSocket,
	frg::default_list_hook<IcmpSocket>, &IcmpSocket::listHook_>> sockets;

IcmpSocket::~IcmpSocket() {
	sockets.erase(this);
}

smarter::shared_ptr<IcmpSocket> IcmpSocket::make_socket(Icmp *parent) {
	auto s = smarter::make_shared<IcmpSocket>(parent);
	sockets.insert(sockets.end(), s.get());
	return s;
}

void Icmp::feedDatagram(smarter::shared_ptr<const Ip4Packet> packet, std::weak_ptr<nic::Link> link) {
	IcmpPacket icmp{ .link = link };
	if (!icmp.parse(std::move(packet))) {
		std::cout << "netserver: broken icmp received" << std::endl;
		return;
	}

	for(auto s : sockets) {
		s->queue_.emplace(IcmpPacket{icmp});
		s->inSeq_ = ++s->currentSeq_;
		s->statusBell_.raise();
	}
}

void Icmp::serveSocket(helix::UniqueLane lane) {
	using protocols::fs::servePassthrough;
	auto sock = IcmpSocket::make_socket(this);
	async::detach(servePassthrough(std::move(lane), std::move(sock),
			&IcmpSocket::ops));
}
