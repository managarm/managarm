
#ifndef LIBNET_UDP_HPP
#define LIBNET_UDP_HPP

#include <string>
#include "ip4.hpp"

namespace libnet {

struct UdpInfo {
	uint16_t sourcePort;
	uint16_t destPort;
};

struct UdpHeader {
	uint16_t source;
	uint16_t destination;
	uint16_t length;
	uint16_t checksum;
};

void sendUdpPacket(NetDevice &device, EthernetInfo link_info, Ip4Info network_info, 
		UdpInfo transport_info, std::string payload);

void receiveUdpPacket(EthernetInfo link_info, Ip4Info network_info, void *buffer, size_t length);

} // namespace libnet

#endif // LIBNET_UDP_HPP

