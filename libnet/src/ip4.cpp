
#include <libnet.hpp>
#include "ip4.hpp"
#include "tcp.hpp"
#include "udp.hpp"

namespace libnet {

void sendIp4Packet(NetDevice &device, EthernetInfo link_info,
		Ip4Info network_info, std::string payload) {
	Ip4Header header;
	header.version_headerLength = (kIp4Version << 4) | (sizeof(Ip4Header) / 4);
	header.dscp_ecn = 0;
	header.length = hostToNet<uint16_t>(sizeof(Ip4Header) + payload.length());
	header.identification = hostToNet<uint16_t>(666);
	header.flags_offset = 0;
	header.ttl = kTtl;
	header.protocol = network_info.protocol;
	header.checksum = 0;
	header.sourceIp = network_info.sourceIp;
	header.targetIp = network_info.destIp;

	Checksum checksum;
	checksum.update(&header, sizeof(Ip4Header));
	header.checksum = hostToNet<uint16_t>(checksum.finish());
	
	std::string packet(sizeof(Ip4Header) + payload.length(), 0);
	memcpy(&packet[0], &header, sizeof(Ip4Header));
	memcpy(&packet[sizeof(Ip4Header)], payload.data(), payload.length());

	sendEthernetPacket(device, link_info, packet);
}

void receiveIp4Packet(EthernetInfo link_info, void *buffer, size_t length) {
	if(length < sizeof(Ip4Header)) {
		printf("    Ip4: Packet is too short!\n");
		return;
	}
	
	auto ip_header = (Ip4Header *)buffer;

	if((ip_header->version_headerLength >> 4) != kIp4Version) {
		printf("    Ip4: Version not supported!\n");
		return;
	}

	size_t header_length = (ip_header->version_headerLength & 0x0F) * 4;
	if(header_length < sizeof(Ip4Header)) {
		printf("    Ip4: headerLength is too small!\n");
		return;
	}else if(netToHost<uint16_t>(ip_header->length) < header_length) {
		printf("    Ip4: totalLength < headerLength!\n");
		return;
	}else if(netToHost<uint16_t>(ip_header->length) != length) {
		printf("    Ip4: totalLength does not match packet length!\n");
		return;
	}

	Ip4Info network_info;
	network_info.sourceIp = ip_header->sourceIp;
	network_info.destIp = ip_header->targetIp;
	network_info.protocol = ip_header->protocol;
	
	void *payload_buffer = (char *)buffer + header_length;
	size_t payload_length = length - header_length;
	
	printf("    Ip4 header. srcIp: %d.%d.%d.%d, destIp: %d.%d.%d.%d, protocol: %d\n",
			network_info.sourceIp.octets[0], network_info.sourceIp.octets[1],
			network_info.sourceIp.octets[2], network_info.sourceIp.octets[3],
			network_info.destIp.octets[0], network_info.destIp.octets[1],
			network_info.destIp.octets[2], network_info.destIp.octets[3],
			network_info.protocol);
	printf("    headerLength: %lu, payloadLength: %lu\n", header_length, payload_length);
	
	auto flags = netToHost<uint16_t>(ip_header->flags_offset);
	auto fragment_offset = flags & kFragmentOffsetMask;
	
	assert(!(flags & kFlagReserved));
	if(fragment_offset != 0) {
		printf("    Ip4: Non-zero fragment offset %d not implemented!\n", fragment_offset);
		return;
	}else if(flags & kFlagMF) {
		printf("    Ip4: MF flag not implemented\n");
		return;
	}

	printf("    flags:");
	if(flags & kFlagDF)
		printf(" DF");
	if(flags & kFlagMF)
		printf(" MF");
	printf("\n");

	if(network_info.protocol == kUdpProtocol) {
		receiveUdpPacket(link_info, network_info, payload_buffer, payload_length);
	} else if(network_info.protocol == kTcpProtocol) {
		receiveTcpPacket(payload_buffer, payload_length);
	} else {
		printf("    Invalid Ip4 protocol type!\n");
	}
}

} // namespace libnet

