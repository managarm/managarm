
#ifndef LIBNET_IP4_HPP
#define LIBNET_IP4_HPP

#include <string>
#include "ethernet.hpp"

namespace libnet {

enum {
	kIp4Version = 4,
	kTtl = 64,
	kUdpProtocol = 17,
	kTcpProtocol = 6
};

struct Ip4Address {
	static Ip4Address broadcast() {
		return Ip4Address(0xFF, 0xFF, 0xFF, 0xFF);
	}

	Ip4Address() : octets{ 0, 0, 0, 0 } { }

	Ip4Address(uint8_t octet0, uint8_t octet1, uint8_t octet2, 
			uint8_t octet3)
	: octets{ octet0, octet1, octet2, octet3 } { }
	
	Ip4Address(uint32_t word)
	: octets{ uint8_t(word >> 24), uint8_t((word >> 16) & 0xFF),
			uint8_t((word >> 8) & 0xFF), uint8_t(word & 0xFF) } { }
	
	bool operator== (const Ip4Address &other) {
		return memcmp(octets, other.octets, 4) == 0;
	}
	bool operator!= (const Ip4Address &other) {
		return !(*this == other);
	}

	uint8_t octets[4];
};

struct Ip4Info {
	Ip4Address sourceIp;
	Ip4Address destIp;
	uint8_t protocol;
};

struct Ip4Header {
	uint8_t version_headerLength;
	uint8_t dscp_ecn;
	uint16_t length;
	uint16_t identification;
	uint16_t flags_offset;
	uint8_t ttl;
	uint8_t protocol;
	uint16_t checksum;
	Ip4Address sourceIp;
	Ip4Address targetIp;
};
static_assert(sizeof(Ip4Header) == 20, "Bad sizeof(Ip4Header)");

struct PseudoIp4Header {
	uint8_t sourceIp[4];
	uint8_t destIp[4];
	uint8_t reserved;
	uint8_t protocol;
	uint16_t length;
};
static_assert(sizeof(PseudoIp4Header) == 12, "Bad sizeof(PseudoIp4Header)");

struct Checksum {
	Checksum()
	: currentSum(0) { }

	void update(const void *buffer, size_t size) {
		auto bytes = reinterpret_cast<const unsigned char *>(buffer);

		if(size == 0)
			return;

		size_t i;
		for(i = 0; i < size - 1; i += 2) {
			uint16_t high = bytes[i], low = bytes[i + 1];
			update((high << 8) | low);
		}
		if(size % 2)
			update(bytes[i]);
	}

	void update(uint16_t value) {
		currentSum += value;
	}

	uint16_t finish() {
		uint32_t result = currentSum;
		while (result >> 16)
			result = (result & 0xFFFF) + (result >> 16);
		assert(result != 0 && result != 0xFFFF); // FIXME: fix this case
		return ~result;
	}

private:
	uint32_t currentSum;
};

void sendIp4Packet(NetDevice &device, EthernetInfo link_info,
		Ip4Info network_info, std::string payload);

} // namespace libnet

#endif // LIBNET_IP4_HPP

