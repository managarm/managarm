#pragma once

#include <arch/bit.hpp>
#include <arch/dma_structs.hpp>
#include <helix/ipc.hpp>
#include <map>
#include <smarter.hpp>
#include <netserver/nic.hpp>
#include <protocols/fs/common.hpp>
#include <set>
#include <cstdint>
#include <memory>
#include <optional>

#include "udp4.hpp"
#include "tcp4.hpp"

#include <netserver/nic.hpp>
#include "fs.bragi.hpp"

enum class IpProto : uint16_t {
	icmp = 1,
	tcp = 6,
	udp = 17,
};

struct CidrAddress {
	uint32_t ip;
	uint8_t prefix;

	inline uint32_t mask() const {
		return (uint64_t(0xFFFFFFFF) << (32 - prefix))
			& 0xFFFFFFFF;
	}

	inline bool sameNet(uint32_t other) const {
		return (other & mask()) == (ip & mask());
	}

	friend bool operator<(const CidrAddress &, const CidrAddress &);
};

struct Ip4Router {
	struct Route {
		inline Route(CidrAddress net, std::weak_ptr<nic::Link> link)
			: network(net), link(link) {}

		CidrAddress network;
		std::weak_ptr<nic::Link> link;
		unsigned int mtu = 0;
		uint32_t gateway = 0;
		unsigned int metric = 0;
		uint32_t source = 0;
		uint8_t scope = 0;
		uint8_t type = 0;
		uint8_t protocol = 0;
		uint32_t flags = 0;
		uint8_t family = 0;

		friend bool operator<(const Route &, const Route &);
	};

	// false if insertion fails
	bool addRoute(Route r);
	std::optional<Route> resolveRoute(uint32_t ip);

	inline const std::set<Route> &getRoutes() const {
		return routes;
	}
private:
	std::set<Route> routes;
};

class Ip4Packet {
	arch::dma_buffer buffer_;
public:
	struct Header {
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

		inline void ensureEndian() {
			auto nendian = [] (auto &x) {
				x = arch::convert_endian<
					arch::endian::big,
					arch::endian::native>(x);
			};
			nendian(length);
			nendian(ident);
			nendian(flags_offset);
			nendian(checksum);
			nendian(source);
			nendian(destination);
		}
	} header;
	static_assert(sizeof(header) == 20, "bad header size");
	arch::dma_buffer_view data;

	inline arch::dma_buffer_view payload() const {
		return data.subview(header.ihl * 4);
	}

	inline arch::dma_buffer_view header_view() const {
		return data.subview(0, header.ihl * 4);
	}

	// assumes frame is a valid view into owner
	bool parse(arch::dma_buffer owner, arch::dma_buffer_view frame);
};

struct Ip4TargetInfo {
	uint32_t remote;
	uint32_t source;
	Ip4Router::Route route;
	std::shared_ptr<nic::Link> link;
};

struct Ip4Socket;
struct Ip4 {
	managarm::fs::Errors serveSocket(helix::UniqueLane lane, int type, int proto, int flags);
	// frame is a view into the owner buffer, stripping away eth bits
	void feedPacket(nic::MacAddress dest, nic::MacAddress src,
		arch::dma_buffer owner, arch::dma_buffer_view frame);

	bool hasIp(uint32_t ip);
	std::shared_ptr<nic::Link> getLink(uint32_t ip);
	void setLink(CidrAddress addr, std::weak_ptr<nic::Link> link);
	std::optional<uint32_t> findLinkIp(uint32_t ipOnNet, nic::Link *link);

	async::result<std::optional<Ip4TargetInfo>> targetByRemote(uint32_t);
	async::result<protocols::fs::Error> sendFrame(Ip4TargetInfo,
		void*, size_t,
		uint16_t);
private:
	std::multimap<int, smarter::shared_ptr<Ip4Socket>> sockets;
	std::map<CidrAddress, std::weak_ptr<nic::Link>> ips;

	Udp4 udp;
	Tcp4 tcp;
};

Ip4 &ip4();
Ip4Router &ip4Router();
