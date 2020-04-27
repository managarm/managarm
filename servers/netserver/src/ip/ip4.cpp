#include "ip4.hpp"

#include "checksum.hpp"
#include <async/doorbell.hpp>
#include <sys/socket.h>
#include <netinet/in.h>
#include <iostream>
#include <iomanip>
#include <arch/bit.hpp>
#include <protocols/fs/server.hpp>
#include <queue>

using namespace protocols::fs;

using Route = Ip4Router::Route;

Ip4Router &ip4Router() {
	static Ip4Router inst;
	return inst;
}

Ip4 &ip4() {
	static Ip4 inst;
	return inst;
}

bool Ip4Router::addRoute(Route r) {
	return routes.emplace(std::move(r)).second;
}

std::optional<Route> Ip4Router::resolveRoute(uint32_t ip) const {
	for (const auto &r : routes) {
		if ((ip & r.mask()) == r.ip) {
			return { r };
		}
	}
	return {};
}

bool operator<(const Route &lhs, const Route &rhs) {
	// bigger MTU is better
	return std::tie(lhs.prefix, lhs.ip, lhs.metric, rhs.mtu) <
		std::tie(rhs.prefix, rhs.ip, rhs.metric, lhs.mtu);
}

class Ip4Packet {
	arch::dma_buffer buffer_;
public:
	struct {
		uint8_t ihl;
		uint8_t tos;
		uint16_t length;

		uint16_t ident;
		uint16_t flags_offset;

		uint8_t ttl;
		uint8_t protocol;
		uint16_t checksum;

		uint32_t source;
		uint32_t destination;
	} header;
	static_assert(sizeof(header) == 20, "bad header size");
	arch::dma_buffer_view data;

	arch::dma_buffer_view capsule() const {
		return data.subview(header.ihl * 4);
	}

	arch::dma_buffer_view header_view() const {
		return data.subview(0, header.ihl * 4);
	}

	// assumes frame is a valid view into owner
	bool parse(arch::dma_buffer owner, arch::dma_buffer_view frame) {
		auto nendian = [] (auto x) -> decltype(x) {
			return arch::convert_endian<
				arch::endian::big, arch::endian::native>(x);
		};
		buffer_ = std::move(owner);
		data = frame;
		if (data.size() < sizeof(header)) {
			return false;
		}
		std::memcpy(&header, data.byte_data(), sizeof(header));
		// 0x40: top four bits are version (4)
		if ((header.ihl & 0xf0) != 0x40) {
			return false;
		}

		header.ihl = nendian(header.ihl & 0x0f);
		header.tos = nendian(header.tos);
		header.length = nendian(header.length);
		// ensure we only access the correct parts of the buffer
		data = data.subview(0, header.length);

		header.ident = nendian(header.ident);
		header.flags_offset = nendian(header.flags_offset);

		header.ttl = nendian(header.ttl);
		header.protocol = nendian(header.protocol);
		header.checksum = nendian(header.checksum);

		header.source = nendian(header.source);
		header.destination = nendian(header.destination);

		Checksum csum;
		csum.update(header_view());
		auto sum = csum.finalize();
		if (sum != 0 && sum != 0xFFFF) {
			std::cout << "netserver: wrong sum: " << sum << std::endl;
			return false;
		}

		return true;
	}

	Ip4Packet() {}
};

struct Ip4Socket {
	explicit Ip4Socket(int proto) : proto(proto) {}

	static async::result<RecvResult> recvmsg(void *obj,
			const char *creds,
			uint32_t flags, void *data, size_t len,
			void *addr_buf, size_t addr_size, size_t max_ctrl_len) {
		auto self = static_cast<Ip4Socket *>(obj);
		while (self->pqueue.empty()) {
			co_await self->bell.async_wait();
		}
		auto element = std::move(self->pqueue.front());
		self->pqueue.pop();
		auto packet = element->data;
		auto copy_size = std::min(packet.size(), len);
		std::memcpy(data, packet.data(), copy_size);
		co_return RecvData { copy_size, 0, {} };
	}

	constexpr static FileOperations ops {
		.recvMsg = &recvmsg
	};
private:
	friend struct Ip4;
	int proto;
	std::queue<smarter::shared_ptr<const Ip4Packet>> pqueue;
	async::doorbell bell;
};

void Ip4::feedPacket(nic::MacAddress dest, nic::MacAddress src,
		arch::dma_buffer owner, arch::dma_buffer_view frame) {
	Ip4Packet hdr;
	if (!hdr.parse(std::move(owner), frame)) {
		std::cout << "netserver: runt, or otherwise invalid, ip4 frame received"
			<< std::endl;
		return;
	}
	auto proto = hdr.header.protocol;

	auto begin = sockets.lower_bound(proto);
	if (begin == sockets.end()) {
		return;
	}
	auto hdrs = smarter::make_shared<const Ip4Packet>(std::move(hdr));
	for (; begin != sockets.end() && begin->first == proto; begin++) {
		begin->second->pqueue.emplace(hdrs);
		begin->second->bell.ring();
	}
}

void Ip4::setLink(uint32_t addr, std::weak_ptr<nic::Link> l) {
	ips.emplace(addr, std::move(l));
}

std::shared_ptr<nic::Link> Ip4::getLink(uint32_t addr) {
	auto iter = ips.find(addr);
	if (iter == ips.end()) {
		return { nullptr };
	}
	auto ptr = iter->second.lock();
	if (!ptr) {
		ips.erase(iter);
		return {};
	}
	return ptr;
}

managarm::fs::Errors Ip4::serveSocket(helix::UniqueLane lane, int type, int proto) {
	using namespace protocols::fs;
	switch (type) {
	case SOCK_RAW: {
		auto sock = smarter::make_shared<Ip4Socket>(proto);
		sockets.emplace(proto, sock);
		async::detach(servePassthrough(std::move(lane),
				sock, &Ip4Socket::ops),
			[this, socket = sock.get()] {
				for (auto i = sockets.begin();
					i != sockets.end();
					i++) {
					if (i->second.get() == socket) {
						sockets.erase(i);
						break;
					}
				}
			});
		return managarm::fs::Errors::SUCCESS;
	}
	default:
		return managarm::fs::Errors::ILLEGAL_ARGUMENT;
	}
}
