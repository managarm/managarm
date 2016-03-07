
#ifndef LIBNET_TCP_HPP
#define LIBNET_TCP_HPP

#include <string>
#include "ip4.hpp"

namespace libnet {

struct TcpInfo {
	uint16_t srcPort;
	uint16_t destPort;
	uint32_t seqNumber;
	uint32_t ackNumber;
	bool ackFlag, rstFlag, synFlag, finFlag;
};

enum TcpFlags {
	kTcpFin = 1,
	kTcpSyn = 2,
	kTcpRst = 4,
	kTcpAck = 16
};

struct TcpHeader {
	uint16_t srcPort;
	uint16_t destPort;
	uint32_t seqNumber;
	uint32_t ackNumber;
	uint16_t flags;
	uint16_t window;
	uint16_t checksum;
	uint16_t urgentPointer;
};

void sendTcpPacket(NetDevice &device, EthernetInfo link_info, Ip4Info network_info, 
		TcpInfo transport_info, std::string payload);

} // namespace libnet

#endif // LIBNET_TCP_HPP

