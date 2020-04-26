#pragma once

#include <helix/ipc.hpp>
#include <map>
#include <smarter.hpp>
#include <netserver/nic.hpp>
#include <set>
#include <cstdint>
#include <memory>
#include <optional>

#include <netserver/nic.hpp>
#include "fs.pb.h"

struct Ip4Socket;

struct Ip4Router {
	struct Route {
		inline Route(uint32_t ip, std::weak_ptr<nic::Link> link,
			uint8_t prefix) : ip(ip), link(link), prefix(prefix) {}

		inline uint32_t mask() const {
			return (uint64_t(0xFFFFFFFF) << (32 - prefix))
				& 0xFFFFFFFF;
		}

		uint32_t ip;
		std::weak_ptr<nic::Link> link;
		uint8_t prefix;

		unsigned int mtu = 0;
		uint32_t gateway = 0;
		unsigned int metric = 0;
		uint32_t source = 0;

		friend bool operator<(const Route &, const Route &);
	};

	// false if insertion fails
	bool addRoute(Route r);
	std::optional<Route> resolveRoute(uint32_t ip) const;
private:
	std::set<Route> routes;
};

struct Ip4 {
	managarm::fs::Errors serveSocket(helix::UniqueLane lane, int type, int proto);
	// frame is a view into the owner buffer, stripping away eth bits
	void feedPacket(nic::MacAddress dest, nic::MacAddress src,
		arch::dma_buffer owner, arch::dma_buffer_view frame);

	Ip4Router &router() {
		return router_;
	}

	std::shared_ptr<nic::Link> getLink(uint32_t ip);
	void setLink(uint32_t ip, std::weak_ptr<nic::Link> link);
private:
	Ip4Router router_;
	std::multimap<int, smarter::shared_ptr<Ip4Socket>> sockets;
	std::map<uint32_t, std::weak_ptr<nic::Link>> ips;
};

Ip4 &ip4();
Ip4Router &ip4Router();
