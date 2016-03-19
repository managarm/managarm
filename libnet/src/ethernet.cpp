
#include <stdio.h>
#include <libnet.hpp>
#include "ethernet.hpp"
#include "ip4.hpp"
#include "arp.hpp"

namespace libnet {

void sendEthernetPacket(NetDevice &device, EthernetInfo link_info, std::string payload) {
	EthernetHeader header;
	header.destAddress = link_info.destMac;
	header.sourceAddress = link_info.sourceMac;
	header.etherType = hostToNet<uint16_t>(link_info.etherType); 
	
	std::string packet(sizeof(EthernetHeader) + payload.length(), 0);
	memcpy(&packet[0], &header, sizeof(EthernetHeader));
	memcpy(&packet[sizeof(EthernetHeader)], payload.data(), payload.length());

	device.sendPacket(packet);
}

void receiveEthernetPacket(void *buffer, size_t length) {
	if(length < sizeof(EthernetHeader)) {
		printf("Ethernet: Packet is too short!\n");
		return;
	}

	auto ethernet_header = (EthernetHeader *)buffer;

	EthernetInfo link_info;
	link_info.sourceMac = ethernet_header->sourceAddress;
	link_info.destMac = ethernet_header->destAddress;
	link_info.etherType = netToHost<uint16_t>(ethernet_header->etherType);
	
	void *payload_buffer = (char *)buffer + sizeof(EthernetHeader);
	size_t payload_length = length - sizeof(EthernetHeader);

	if(link_info.destMac != localMac && link_info.destMac != MacAddress::broadcast()) {
//		printf("Destination mismatch\n");
		return;
	}
	
	printf("Ethernet frame. srcMac: %x:%x:%x:%x:%x:%x, destMac: %x:%x:%x:%x:%x:%x, etherType: 0x%x\n",
			link_info.sourceMac.octets[0], link_info.sourceMac.octets[1],
			link_info.sourceMac.octets[2], link_info.sourceMac.octets[3],
			link_info.sourceMac.octets[4], link_info.sourceMac.octets[5],
			link_info.destMac.octets[0], link_info.destMac.octets[1],
			link_info.destMac.octets[2], link_info.destMac.octets[3],
			link_info.destMac.octets[4], link_info.destMac.octets[5],
			link_info.etherType);
	
	if(link_info.etherType == kEtherIp4) {
		receiveIp4Packet(link_info, payload_buffer, payload_length);
	} else if(link_info.etherType == kEtherArp) {
		receiveArpPacket(payload_buffer, payload_length);
	} else {
		printf("    Ethernet: Unexpected etherType 0x%X!\n", link_info.etherType);
	}
}

} // namespace libnet

