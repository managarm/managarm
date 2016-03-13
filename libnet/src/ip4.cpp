
#include <libnet.hpp>
#include "ip4.hpp"

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

} // namespace libnet

