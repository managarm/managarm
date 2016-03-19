
#include <libnet.hpp>
#include "arp.hpp"

namespace libnet {

void receiveArpPacket(void *buffer, size_t length) {
	printf("sizeof(ArpPacket): %lu\n", length);
	if(length != sizeof(ArpPacket)) {
		printf("Length is not the actual ArpPacket length!\n");
// 		return;
	}
	
	auto arp_packet = (ArpPacket *)buffer;
	localMac = arp_packet->targetHw;
	printf("localMac: %d:%d:%d:%d:%d:%d\n", localMac.octets[0], localMac.octets[1],
			localMac.octets[2], localMac.octets[3], localMac.octets[4], localMac.octets[5]);		
	routerMac = arp_packet->senderHw;
	printf("routerMac: %d:%d:%d:%d:%d:%d\n", routerMac.octets[0], routerMac.octets[1],
			routerMac.octets[2], routerMac.octets[3], routerMac.octets[4], routerMac.octets[5]);
	
	printf("hwtype: %i\n", netToHost<uint16_t>(arp_packet->hwType));
	printf("prototype: %i\n", netToHost<uint16_t>(arp_packet->protoType));
	printf("hwlength: %i\n", arp_packet->hwLength);
	printf("protolength: %i\n", arp_packet->protoLength);
	printf("operation: %i\n", netToHost<uint16_t>(arp_packet->operation));

	sendDnsRequest();
	//tcpSocket.connect();
}

void sendArpRequest() {
	std::string packet;
	packet.resize(sizeof(ArpPacket));

	ArpPacket arp_packet;
	arp_packet.hwType = hostToNet<uint16_t>(1);
	arp_packet.protoType = hostToNet<uint16_t>(0x800);
	arp_packet.hwLength = 6;
	arp_packet.protoLength = 4;
	arp_packet.operation = hostToNet<uint16_t>(1);
	arp_packet.senderHw = localMac;
	arp_packet.senderProto = localIp;
	arp_packet.targetHw = MacAddress::broadcast();
	arp_packet.targetProto = routerIp;

	memcpy(&packet[0], &arp_packet, sizeof(ArpPacket));
	
	EthernetInfo ethernet_info;
	ethernet_info.sourceMac = localMac;
	ethernet_info.destMac = MacAddress::broadcast();
	ethernet_info.etherType = kEtherArp;

	sendEthernetPacket(*globalDevice, ethernet_info, packet);
}

} // namespace libnet

