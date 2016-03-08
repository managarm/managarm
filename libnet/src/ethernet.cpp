
#include <libnet.hpp>
#include "ethernet.hpp"

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

} // namespace libnet

