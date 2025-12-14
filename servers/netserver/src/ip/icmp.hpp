#pragma once

#include <async/queue.hpp>
#include <core/id-allocator.hpp>
#include <helix/ipc.hpp>
#include <netserver/nic.hpp>
#include <smarter.hpp>
#include <sys/time.h>

#include "ip4.hpp"

class Ip4Packet;
struct IcmpSocket;

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

	bool parse(smarter::shared_ptr<const Ip4Packet> packet);

	smarter::shared_ptr<const Ip4Packet> packet;
	struct timeval recvTimestamp;

	std::weak_ptr<nic::Link> link;
};

struct Icmp {
	Icmp();

	void feedDatagram(smarter::shared_ptr<const Ip4Packet>, std::weak_ptr<nic::Link> link);
	void serveSocket(helix::UniqueLane ctrlLane, helix::UniqueLane ptLane);

private:
	async::result<void> dispatchIcmp_();

	async::queue<IcmpPacket, frg::stl_allocator> queue_;
};
