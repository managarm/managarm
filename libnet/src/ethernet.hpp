
#ifndef LIBNET_ETHERNET_HPP
#define LIBNET_ETHERNET_HPP

#include <string>

namespace libnet {

struct MacAddress {
	static MacAddress broadcast() {
		return MacAddress(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);
	}

	MacAddress() : octets{ 0, 0, 0, 0, 0, 0 } { }

	MacAddress(uint8_t octet0, uint8_t octet1, uint8_t octet2, 
			uint8_t octet3, uint8_t octet4, uint8_t octet5)
	: octets{ octet0, octet1, octet2, octet3, octet4, octet5 } { }

	uint8_t octets[6];
};

struct EthernetInfo {
	MacAddress destMac;
	MacAddress sourceMac;
	uint16_t etherType;
};

struct EthernetHeader {
	uint8_t destAddress[6];
	uint8_t sourceAddress[6];
	uint16_t etherType;
};

void sendEthernetPacket(NetDevice &device, EthernetInfo link_info, std::string payload);

} // namespace libnet

#endif // LIBNET_ETHERNET_HPP

