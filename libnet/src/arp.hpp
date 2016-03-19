
#ifndef LIBNET_ARP_HPP
#define LIBNET_ARP_HPP

#include <stdio.h>
#include <string>
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

void receiveArpPacket(void *buffer, size_t length);

void sendArpRequest();

} // namespace libnet

#endif // LIBNET_ARP_HPP

