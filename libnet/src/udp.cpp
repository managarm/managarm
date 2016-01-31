
#include <libnet.hpp>
#include "udp.hpp"

namespace libnet {

void sendUdpPacket(NetDevice &device, EthernetInfo link_info, Ip4Info network_info, 
		UdpInfo transport_info, std::string payload) {
	UdpHeader header;
	header.source = hostToNet<uint16_t>(transport_info.sourcePort);
	header.destination = hostToNet<uint16_t>(transport_info.destPort);
	header.length = hostToNet<uint16_t>(sizeof(UdpHeader) + payload.length());
	header.checksum = 0;

	// calculate the UDP checksum
	PseudoIp4Header pseudo;
	memcpy(pseudo.sourceIp, network_info.sourceIp.octets, 4);
	memcpy(pseudo.destIp, network_info.destIp.octets, 4);
	pseudo.reserved = 0;
	pseudo.protocol = kUdpProtocol;
	pseudo.length = hostToNet<uint16_t>(sizeof(UdpHeader) + payload.length());

	Checksum udp_checksum;
	udp_checksum.update(&pseudo, sizeof(PseudoIp4Header));
	udp_checksum.update(&header, sizeof(UdpHeader));
	udp_checksum.update(payload.data(), payload.size());
	header.checksum = hostToNet<uint16_t>(udp_checksum.finish());

	std::string packet(sizeof(UdpHeader) + payload.length(), 0);
	memcpy(&packet[0], &header, sizeof(UdpHeader));
	memcpy(&packet[sizeof(UdpHeader)], payload.data(), payload.length());

	sendIp4Packet(device, link_info, network_info, packet);
}

} // namespace libnet

