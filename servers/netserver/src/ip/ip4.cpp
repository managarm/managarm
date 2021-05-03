#include "ip4.hpp"

#include "arp.hpp"
#include "checksum.hpp"
#include <async/recurring-event.hpp>
#include <sys/socket.h>
#include <netinet/in.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <iomanip>
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

std::optional<Route> Ip4Router::resolveRoute(uint32_t ip) {
	for (auto i = routes.begin(); i != routes.end(); i++) {
		const auto &r = *i;
		if (r.network.sameNet(ip)) {
			if (r.link.expired()) {
				i = routes.erase(i);
				continue;
			}
			return { r };
		}
	}
	return {};
}

bool operator<(const CidrAddress &lhs, const CidrAddress &rhs) {
	return std::tie(lhs.prefix, lhs.ip) < std::tie(lhs.prefix, lhs.ip);
}

bool operator<(const Route &lhs, const Route &rhs) {
	// bigger MTU is better, and hence sorts lower
	return std::tie(lhs.network, lhs.metric, rhs.mtu) <
		std::tie(rhs.network, rhs.metric, lhs.mtu);
}

bool Ip4Packet::parse(arch::dma_buffer owner, arch::dma_buffer_view frame) {
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

	header.ensureEndian();

	header.ihl = header.ihl & 0x0f;
	// ensure we only access the correct parts of the buffer
	data = data.subview(0, header.length);

	Checksum csum;
	csum.update(header_view());
	auto sum = csum.finalize();
	if (sum != 0 && sum != 0xFFFF) {
		std::cout << "netserver: wrong sum: " << sum << std::endl;
		return false;
	}

	return true;
}

namespace {
auto checkAddress(const void *addr_ptr, size_t addr_len, uint32_t &ip) {
	struct sockaddr_in addr;
	if (addr_len < sizeof(addr)) {
		return protocols::fs::Error::illegalArguments;
	}
	std::memcpy(&addr, addr_ptr, sizeof(addr));
	if (addr.sin_family != AF_INET) {
		return protocols::fs::Error::afNotSupported;
	}
	ip = addr.sin_addr.s_addr;
	return protocols::fs::Error::none;
}
}

struct Ip4Socket {
	explicit Ip4Socket(int proto) : proto(proto) {}

	static async::result<RecvResult> recvmsg(void *obj,
			const char *creds,
			uint32_t flags, void *data, size_t len,
			void *addr_buf, size_t addr_size, size_t max_ctrl_len) {
		using arch::convert_endian;
		using arch::endian;
		auto self = static_cast<Ip4Socket *>(obj);
		while (self->pqueue.empty()) {
			co_await self->bell.async_wait();
		}
		auto element = std::move(self->pqueue.front());
		self->pqueue.pop();
		auto packet = element->data;
		auto copy_size = std::min(packet.size(), len);
		std::memcpy(data, packet.data(), copy_size);
		sockaddr_in addr {
			.sin_family = AF_INET,
			.sin_port = convert_endian<endian::big>(element->header.protocol),
			.sin_addr = { convert_endian<endian::big>(element->header.source) }
		};
		std::memset(addr_buf, 0, addr_size);
		std::memcpy(addr_buf, &addr, std::min(addr_size, sizeof(addr)));
		co_return RecvData { copy_size, sizeof(addr), {} };
	}

	static async::result<frg::expected<protocols::fs::Error, size_t>> sendmsg(void *obj,
			const char *creds, uint32_t flags,
			void *data, size_t len,
			void *addr_ptr, size_t addr_size,
			std::vector<uint32_t> fds);

	static async::result<Error> connect(void* obj,
			const char *creds,
			const void *addr_ptr, size_t addr_size) {
		auto self = static_cast<Ip4Socket *>(obj);
		uint32_t ip;
		if (auto e = checkAddress(addr_ptr, addr_size, ip);
			e != protocols::fs::Error::none) {
			co_return e;
		}
		
		// TODO(arsen): check other broadcast addresses too
		if (ip == INADDR_ANY) {
			co_return protocols::fs::Error::accessDenied;
		}
		self->remote = ip;
		co_return protocols::fs::Error::none;
	}

	constexpr static FileOperations ops {
		.connect = &connect,
		.recvMsg = &recvmsg,
		.sendMsg = &sendmsg,
	};
private:
	friend struct Ip4;
	int proto;
	uint32_t remote = 0;
	std::queue<smarter::shared_ptr<const Ip4Packet>> pqueue;
	async::recurring_event bell;
};

async::result<frg::expected<protocols::fs::Error, size_t>> Ip4Socket::sendmsg(void *obj,
		const char *creds, uint32_t flags,
		void *data, size_t len,
		void *addr_ptr, size_t addr_size,
		std::vector<uint32_t> fds) {
	using arch::convert_endian;
	using arch::endian;
	auto self = static_cast<Ip4Socket *>(obj);
	uint32_t address;
	if (addr_size != 0) {
		if (auto e = checkAddress(addr_ptr, addr_size, address);
			e != protocols::fs::Error::none) {
			co_return e;
		}
	} else {
		address = self->remote;
	}

	address = convert_endian<endian::big>(address);

	if (address == 0) {
		std::cout << "netserver: needs destination" << std::endl;
		co_return protocols::fs::Error::destAddrRequired;
	}

	if (address == INADDR_BROADCAST) {
		co_return protocols::fs::Error::accessDenied;
	}

	auto ti = co_await ip4().targetByRemote(address);
	if (!ti) {
		co_return protocols::fs::Error::netUnreachable;
	}

	auto error = co_await ip4().sendFrame(std::move(*ti),
		data, len, self->proto);
	if (error != protocols::fs::Error::none) {
		co_return error;
	}

	co_return len;
}

async::result<std::optional<Ip4TargetInfo>>
Ip4::targetByRemote(uint32_t remote) {
	auto oroute = ip4Router().resolveRoute(remote);
	if (!oroute) {
		std::cout << "netserver: net unreachable" << std::endl;
		co_return std::nullopt;
	}

	auto target = oroute->link.lock();
	if (!target) {
		std::cout << "netserver: route link disappeared"
			<< std::endl;
		// TODO(arsen): remove route too
		co_return std::nullopt;
	}

	auto source = oroute->source;
	if (source == 0) {
		auto elvis = oroute->gateway ? oroute->gateway : remote;
		source = findLinkIp(elvis, target.get()).value_or(0);
	}
	if (source == 0) {
		std::cout << "netserver: could not find same network ip for "
			<< std::hex << std::setw(8) << remote << std::dec << std::endl;
		co_return std::nullopt;
	}

	co_return Ip4TargetInfo { remote, source, *oroute, std::move(target) };
}

bool Ip4::hasIp(uint32_t addr) {
	return std::any_of(ips.cbegin(), ips.cend(),
		[addr] (auto &x) {
			return x.first.ip == addr;
		});
}

async::result<protocols::fs::Error> Ip4::sendFrame(Ip4TargetInfo ti,
		void *data, size_t len, uint16_t proto) {
	using arch::convert_endian;
	using arch::endian;

	// TODO(arsen): fragmentation
	// calculate header size
	size_t header_size = sizeof(Ip4Packet::Header);
	size_t packet_size = len + header_size;
	// TODO(arsen): options
	if (ti.route.mtu != 0 && ti.route.mtu < packet_size) {
		std::cout << "netserver: cant fragment 1" << std::endl;
		co_return protocols::fs::Error::messageSize;
	}

	auto &target = ti.link;
	if (target->mtu < packet_size) {
		std::cout << "netserver: cant fragment 2" << std::endl;
		co_return protocols::fs::Error::messageSize;
	}

	auto macTarget = ti.route.gateway;
	if (macTarget == 0) {
		macTarget = ti.remote;
	}

	auto mac = co_await neigh4().tryResolve(macTarget, ti.source);
	if (!mac) {
		co_return protocols::fs::Error::hostUnreachable;
	}

	Ip4Packet::Header hdr;
	// TODO(arsen): options
	hdr.ihl = 0x45;
	hdr.tos = 0;
	hdr.length = packet_size;
	// TODO(arsen): fragmentation
	hdr.flags_offset = 0;
	hdr.ttl = 64;
	hdr.protocol = proto;
	// filled out later, 0 for purposes of computation
	hdr.checksum = 0;
	hdr.source = ti.source;
	hdr.destination = ti.remote;

	hdr.ensureEndian();

	Checksum chk;
	// TODO(arsen): accomodate for options
	chk.update(reinterpret_cast<void *>(&hdr), sizeof(hdr));
	hdr.checksum = convert_endian<endian::big>(chk.finalize());

	auto fb = target->allocateFrame(*mac, nic::ETHER_TYPE_IP4, packet_size);

	std::memcpy(fb.payload.data(), &hdr, sizeof(hdr));
	std::memcpy(fb.payload.subview(header_size).byte_data(), data, len);

	co_await target->send(std::move(fb.frame));
	co_return protocols::fs::Error::none;
}

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
	if (begin == sockets.end()
			&& proto != static_cast<uint16_t>(IpProto::udp)
			&& proto != static_cast<uint16_t>(IpProto::tcp)) {
		return;
	}

	auto hdrs = smarter::make_shared<const Ip4Packet>(std::move(hdr));

	switch (static_cast<IpProto>(proto)) {
	case IpProto::udp: udp.feedDatagram(hdrs); break;
	case IpProto::tcp: tcp.feedDatagram(hdrs); break;
	default: break;
	}

	for (; begin != sockets.end() && begin->first == proto; begin++) {
		begin->second->pqueue.emplace(hdrs);
		begin->second->bell.raise();
	}
}

void Ip4::setLink(CidrAddress addr, std::weak_ptr<nic::Link> l) {
	ips.emplace(addr, std::move(l));
}

std::shared_ptr<nic::Link> Ip4::getLink(uint32_t addr) {
	auto iter = std::find_if(ips.begin(), ips.end(),
		[addr] (const auto &e) { return e.first.ip == addr; });
	if (iter == ips.end()) {
		return {};
	}
	auto ptr = iter->second.lock();
	if (!ptr) {
		ips.erase(iter);
		return {};
	}
	return ptr;
}

std::optional<uint32_t> Ip4::findLinkIp(uint32_t ipOnNet, nic::Link *link) {
	for (auto &entry : ips) {
		if (!entry.second.expired() && entry.first.sameNet(ipOnNet)) {
			auto o = entry.second.lock();
			if (o.get() == link) {
				return entry.first.ip;
			}
		}
	}
	return {};
}

managarm::fs::Errors Ip4::serveSocket(helix::UniqueLane lane, int type, int proto, int flags) {
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
	case SOCK_DGRAM:
		udp.serveSocket(std::move(lane));
		return managarm::fs::Errors::SUCCESS;
	case SOCK_STREAM:
		tcp.serveSocket(flags, std::move(lane));
		return managarm::fs::Errors::SUCCESS;
	default:
		return managarm::fs::Errors::ILLEGAL_ARGUMENT;
	}
}
