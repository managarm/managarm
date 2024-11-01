#pragma once

#include <helix/ipc.hpp>
#include <map>
#include <netserver/nic.hpp>
#include <smarter.hpp>

class Ip4Packet;

struct Endpoint {
	uint32_t addr = 0;
	uint16_t port = 0;

	Endpoint &operator=(struct sockaddr_in sa);
	void ensureEndian();
};

bool operator<(const Endpoint &l, const Endpoint &r);

struct Udp4Socket;
struct Udp4 {
	void feedDatagram(smarter::shared_ptr<const Ip4Packet>, std::weak_ptr<nic::Link> link);
	bool tryBind(smarter::shared_ptr<Udp4Socket> socket, Endpoint addr);
	bool unbind(Endpoint remote);
	void serveSocket(helix::UniqueLane lane);

  private:
	std::map<Endpoint, smarter::shared_ptr<Udp4Socket>> binds;
};
