
#ifndef LIBNET_ARP_HPP
#define LIBNET_ARP_HPP

#include <stdio.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <frigg/callback.hpp>

#include "ethernet.hpp"
#include "ip4.hpp"

namespace libnet {

struct ArpPacket {
	uint16_t hwType;
	uint16_t protoType;
	uint8_t hwLength, protoLength;
	uint16_t operation;
	MacAddress senderHw;
	Ip4Address senderProto;
	MacAddress targetHw;
	Ip4Address targetProto;
};

struct ArpEntry {
	ArpEntry();

	Ip4Address address;
	MacAddress result;
	bool finished;

	std::vector<frigg::CallbackPtr<void(MacAddress)>> callbacks;
};

void arpLookup(Ip4Address address, frigg::CallbackPtr<void(MacAddress)> callback);

void receiveArpPacket(void *buffer, size_t length);

} // namespace libnet

#endif // LIBNET_ARP_HPP

