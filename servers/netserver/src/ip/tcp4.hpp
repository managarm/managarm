#pragma once

#include <helix/ipc.hpp>
#include <smarter.hpp>
#include <map>

class Ip4Packet;

struct TcpEndpoint {
	friend bool operator<(const TcpEndpoint &l, const TcpEndpoint &r) {
		return std::tie(l.port, l.ipAddress) < std::tie(r.port, r.ipAddress);
	}

	uint32_t ipAddress = 0;
	uint16_t port = 0;
};

struct Tcp4Socket;

struct Tcp4 {
	void feedDatagram(smarter::shared_ptr<const Ip4Packet>);
	bool tryBind(smarter::shared_ptr<Tcp4Socket> socket, TcpEndpoint ipAddress);
	bool unbind(TcpEndpoint remote);
	void serveSocket(helix::UniqueLane lane);

private:
	std::map<TcpEndpoint, smarter::shared_ptr<Tcp4Socket>> binds;
};
