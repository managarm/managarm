
#include <libnet.hpp>
#include "udp.hpp"
#include "usernet.hpp"

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

void receiveUdpPacket(EthernetInfo link_info, Ip4Info network_info, void *buffer, size_t length) {
	if(length < sizeof(UdpHeader)) {
		printf("Udp packet is too short!\n");
		return;
	}

	auto udp_header = (UdpHeader *)buffer;
	
	printf("        UDP header. srcPort: %d, destPort: %d\n", netToHost<uint16_t>(udp_header->source),
			netToHost<uint16_t>(udp_header->destination));

	if(netToHost<uint16_t>(udp_header->length) != length) {
		printf("        UDP: Invalid length!\n");
		return;
	}

	void *payload_buffer = (char *)buffer + sizeof(UdpHeader);
	size_t payload_length = length - sizeof(UdpHeader);

	if(netToHost<uint16_t>(udp_header->source) == 67
			&& netToHost<uint16_t>(udp_header->destination) == 68)
		receivePacket(link_info, network_info, payload_buffer, payload_length);
	if(netToHost<uint16_t>(udp_header->source) == 53)
		receiveDnsPacket(payload_buffer, payload_length);
}

} // namespace libnet

