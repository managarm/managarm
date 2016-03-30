
#include <libnet.hpp>
#include "arp.hpp"

namespace libnet {

std::unordered_map<Ip4Address, ArpEntry *> arpCache;

ArpEntry::ArpEntry()
: finished(false) { }

void arpLookup(Ip4Address address, frigg::CallbackPtr<void(MacAddress)> callback) {	
	auto it = arpCache.find(address);
	if(it != arpCache.end()) {
		if(it->second->finished) {
			callback(it->second->result);
		}else{
			it->second->callbacks.push_back(callback);
		}
	}else{
		auto entry = new ArpEntry;
		entry->address = address;
		entry->callbacks.push_back(callback);
		arpCache.insert({ address, entry });
	
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
		arp_packet.targetProto = address;

		memcpy(&packet[0], &arp_packet, sizeof(ArpPacket));
		
		EthernetInfo ethernet_info;
		ethernet_info.sourceMac = localMac;
		ethernet_info.destMac = MacAddress::broadcast();
		ethernet_info.etherType = kEtherArp;

		sendEthernetPacket(*globalDevice, ethernet_info, packet);
	}
}

void receiveArpPacket(void *buffer, size_t length) {
	printf("    sizeof(ArpPacket): %lu\n", length);
	if(length != sizeof(ArpPacket)) {
		printf("    Length is not the actual ArpPacket length!\n");
// 		return;
	}
	
	auto arp_packet = (ArpPacket *)buffer;
	
	printf("    hwType: %i, protoType: %i, hwLength: %i, protoLength: %i, operation: %i \n",
			netToHost<uint16_t>(arp_packet->hwType), netToHost<uint16_t>(arp_packet->protoType),
			arp_packet->hwLength, arp_packet->protoLength, netToHost<uint16_t>(arp_packet->operation));

	printf("    senderMac: %d:%d:%d:%d:%d:%d\n", arp_packet->senderHw.octets[0],
			arp_packet->senderHw.octets[1], arp_packet->senderHw.octets[2],
			arp_packet->senderHw.octets[3], arp_packet->senderHw.octets[4],
			arp_packet->senderHw.octets[5]);
	printf("    targetMac: %d:%d:%d:%d:%d:%d\n", arp_packet->targetHw.octets[0],
			arp_packet->targetHw.octets[1], arp_packet->targetHw.octets[2],
			arp_packet->targetHw.octets[3], arp_packet->targetHw.octets[4],
			arp_packet->targetHw.octets[5]);		
	
	
	auto entry_it = arpCache.find(arp_packet->senderProto);
	if(entry_it != arpCache.end()) {
		ArpEntry *entry = entry_it->second;
		entry->finished = true;
		entry->result = arp_packet->targetHw;
		for(auto it = entry->callbacks.begin(); it != entry->callbacks.end(); ++it)
			(*it)(arp_packet->senderHw);
	}

	//tcpSocket.connect();
}

} // namespace libnet

