
#ifndef LIBNET_USERNET_HPP
#define LIBNET_USERNET_HPP

#include <stdio.h>
#include <string>
#include "ethernet.hpp"
#include "ip4.hpp"

namespace libnet {

enum {
	kBootpNull = 0,
	kBootpEnd = 255,

	kBootpSubnet = 1,
	kBootpRouters = 3,
	kBootpDns = 6,

	kDhcpRequestedIp = 50,
	kDhcpLeaseTime = 51,
	kDhcpMessageType = 53,
	kDhcpServer = 54
};

enum DhcpState {
	kDefaultState,
	kDiscoverySent,
	kRequestSent,
	kAckReceived
};

enum {
	kTypeDiscover = 1,
	kTypeOffer = 2,
	kTypeRequest = 3,
	kTypeDecline = 4,
	kTypeAck = 5,
	kTypeNak = 6,
	kTypeRelease = 7,
	kTypeInform = 8
};

enum {
	// bits of the BOOTP flags field
	kDhcpBroadcast = 0x8000,

	// dhcp magic option
	kDhcpMagic = 0x63825363
};

struct DhcpHeader {
	uint8_t op;
	uint8_t htype;
	uint8_t hlen;
	uint8_t hops;
	uint32_t transaction;
	uint16_t secondsSinceBoot;
	uint16_t flags;
	Ip4Address clientIp;
	Ip4Address assignedIp;
	Ip4Address serverIp;
	Ip4Address gatewayIp;
	uint8_t clientHardware[16];
	uint8_t serverHost[64];
	uint8_t file[128];
	uint32_t magic; // move this out of DhcpHeader
};

extern DhcpState dhcpState;
extern uint32_t dhcpTransaction;

void receivePacket(EthernetInfo link_info, Ip4Info network_info, void *buffer, size_t length);

void receiveDnsPacket(void *buffer, size_t length);

void sendDnsRequest();

void sendDhcpDiscover(NetDevice &device);

std::string readDnsName(void *packet, uint8_t *&it);

} // namespace libnet

#endif // LIBNET_USERNET_HPP

