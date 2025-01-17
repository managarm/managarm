#pragma once

#include <core/id-allocator.hpp>
#include <helix/ipc.hpp>
#include <netserver/nic.hpp>
#include <smarter.hpp>

class Ip4Packet;
struct IcmpSocket;

struct Icmp {
	void feedDatagram(smarter::shared_ptr<const Ip4Packet>, std::weak_ptr<nic::Link> link);
	void serveSocket(helix::UniqueLane lane);
};
