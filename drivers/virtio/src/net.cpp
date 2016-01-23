
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include "net.hpp"

template<typename T>
T hostToNet(T value);

template<typename T>
T netToHost(T value);

template<>
uint16_t hostToNet(uint16_t value) {
	return __builtin_bswap16(value);
}
template<>
uint32_t hostToNet(uint32_t value) {
	return __builtin_bswap32(value);
}

template<>
uint16_t netToHost(uint16_t value) {
	return __builtin_bswap16(value);
}

extern helx::EventHub eventHub;

enum {
	kEtherIp4 = 0x0800,
	kEtherArp = 0x0806
};

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

struct ArpPacket {
	uint16_t hwType;
	uint16_t protoType;
	uint8_t hwLength, protoLength;
	uint16_t operation;
	uint8_t senderHw[6];
	uint8_t senderProto[4];
	uint8_t targetHw[6];
	uint8_t targetProto[4];
};

enum {
	kIp4Version = 4,
	kTtl = 64,
	kUdpProtocol = 17
};

struct Ip4Address {
	Ip4Address() : octets{ 0, 0, 0, 0 } { }

	Ip4Address(uint8_t octet0, uint8_t octet1, uint8_t octet2, 
			uint8_t octet3)
	: octets{ octet0, octet1, octet2, octet3 } { }
	
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
	uint8_t sourceIp[4];
	uint8_t targetIp[4];
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

struct UdpInfo {
	uint16_t sourcePort;
	uint16_t destPort;
};

struct UdpHeader {
	uint16_t source;
	uint16_t destination;
	uint16_t length;
	uint16_t checksum;
};

struct DhcpDiscover {
	uint8_t op;
	uint8_t htype;
	uint8_t hlen;
	uint8_t hops;
	uint32_t xid;
	uint16_t secs;
	uint16_t flags;
	uint32_t ciaddr;
	uint32_t yiaddr;
	uint32_t siaddr;
	uint32_t giaddr;
	uint8_t chaddr[16];
	uint8_t sname[64];
	uint8_t file[128];
	uint32_t magic;
};

struct Checksum {
	Checksum()
	: currentSum(0) { }

	void update(const void *buffer, size_t size) {
		auto bytes = reinterpret_cast<const unsigned char *>(buffer);

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

void sendEthernetPacket(virtio::net::Device &device, EthernetInfo link_info, std::string payload) {
	EthernetHeader header;
	memcpy(header.destAddress, link_info.destMac.octets, 6);
	memcpy(header.sourceAddress, link_info.sourceMac.octets, 6);
	header.etherType = hostToNet(link_info.etherType); 
	
	std::string packet(sizeof(EthernetHeader) + payload.length(), 0);
	memcpy(&packet[0], &header, sizeof(EthernetHeader));
	memcpy(&packet[sizeof(EthernetHeader)], payload.data(), payload.length());

	device.sendPacket(packet);
}

void sendIp4Packet(virtio::net::Device &device, EthernetInfo link_info,
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
	memcpy(header.sourceIp, network_info.sourceIp.octets, 4);
	memcpy(header.targetIp, network_info.destIp.octets, 4);

	Checksum checksum;
	checksum.update(&header, sizeof(Ip4Header));
	header.checksum = hostToNet<uint16_t>(checksum.finish());
	
	std::string packet(sizeof(Ip4Header) + payload.length(), 0);
	memcpy(&packet[0], &header, sizeof(Ip4Header));
	memcpy(&packet[sizeof(Ip4Header)], payload.data(), payload.length());

	sendEthernetPacket(device, link_info, packet);
}

void sendUdpPacket(virtio::net::Device &device, EthernetInfo link_info, Ip4Info network_info, 
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

namespace virtio {
namespace net {

void *receiveBuffer;
void *transmitBuffer;

Device::Device()
		: receiveQueue(*this, 0), transmitQueue(*this, 1) { }

void Device::sendPacket(std::string packet) {
	size_t tx_header_index = transmitQueue.lockDescriptor();
	size_t tx_packet_index = transmitQueue.lockDescriptor();

	auto header = (VirtHeader *)transmitBuffer;
	header->flags = 0;
	header->gsoType = 0;
	header->hdrLen = 0;
	header->gsoSize = 0;
	header->csumStart = 0;
	header->csumOffset = 0;
	
	memcpy((char *)transmitBuffer + sizeof(VirtHeader), packet.data(), packet.length());
	
	// setup a descriptor for the virtio header
	uintptr_t tx_header_physical;
	HEL_CHECK(helPointerPhysical(transmitBuffer, &tx_header_physical));
	
	VirtDescriptor *tx_header_desc = transmitQueue.accessDescriptor(tx_header_index);
	tx_header_desc->address = tx_header_physical;
	tx_header_desc->length = sizeof(VirtHeader);
	tx_header_desc->flags = VIRTQ_DESC_F_NEXT;
	tx_header_desc->next = tx_packet_index;

	// setup a descriptor for the packet
	VirtDescriptor *tx_packet_desc = transmitQueue.accessDescriptor(tx_packet_index);
	tx_packet_desc->address = tx_header_physical + sizeof(VirtHeader);
	tx_packet_desc->length = packet.length();
	tx_packet_desc->flags = 0;

	transmitQueue.postDescriptor(tx_header_index);
	transmitQueue.notifyDevice();
}

void Device::testDevice() {
	printf("testDevice()\n");
	
	MacAddress local_mac;
	for(size_t i = 0; i < 6; i++)
		local_mac.octets[i] = readConfig8(i);
	
//#############################################################
//				DHCP	
//#############################################################

	std::string packet;
	packet.resize(sizeof(DhcpDiscover) + 4);

	DhcpDiscover dhcp_discover;
	dhcp_discover.op = 1;
	dhcp_discover.htype = 1;
	dhcp_discover.hlen = 6;
	dhcp_discover.hops = 0;
	dhcp_discover.xid = hostToNet<uint32_t>(3);
	dhcp_discover.secs = hostToNet<uint16_t>(0);
	dhcp_discover.flags = hostToNet<uint16_t>(0x8000);
	dhcp_discover.ciaddr = 0;
	dhcp_discover.yiaddr = 0;
	dhcp_discover.siaddr = 0;
	dhcp_discover.giaddr = 0;
	memset(dhcp_discover.chaddr, 0, 16);
	memcpy(dhcp_discover.chaddr, local_mac.octets, 6);
	memset(dhcp_discover.sname, 0, 64);
	memset(dhcp_discover.file, 0, 128);
	dhcp_discover.magic = hostToNet<uint32_t>(0x63825363);
	memcpy(&packet[0], &dhcp_discover, sizeof(DhcpDiscover));

	auto dhcp_options = &packet[sizeof(DhcpDiscover)];
	dhcp_options[0] = 53;
	dhcp_options[1] = 1;
	dhcp_options[2] = 1;
	dhcp_options[3] = 0xFF;
	
	EthernetInfo ethernet_info;
	ethernet_info.sourceMac = local_mac;
	ethernet_info.destMac = MacAddress::broadcast();
	ethernet_info.etherType = kEtherIp4;

	Ip4Info ip_info;
	ip_info.sourceIp = Ip4Address(0, 0, 0, 0);
	ip_info.destIp = Ip4Address(0xFF, 0xFF, 0xFF, 0xFF);
	ip_info.protocol = kUdpProtocol;

	UdpInfo udp_info;
	udp_info.sourcePort = 68;
	udp_info.destPort = 67;
	
	sendUdpPacket(*this, ethernet_info, ip_info, udp_info, packet);

//#############################################################
//					Packet & UDP
//#############################################################

/*	uint8_t targetMac[6] = { 0x52, 0x55, 0x0A, 0x00, 0x02, 0x02 };

	auto ethernet = (EthernetHeader *)((char *)transmitBuffer + sizeof(VirtHeader));
	memcpy(ethernet->destAddress, targetMac, 6);
	memcpy(ethernet->sourceAddress, local_mac, 6);
	ethernet->etherType = hostToNet<uint16_t>(kEtherIp4);

	auto ip_header = (Ip4Header *)((char *)transmitBuffer + sizeof(VirtHeader) +
			sizeof(EthernetHeader));
	
	ip_header->version_headerLength = (kIp4Version << 4) | (sizeof(Ip4Header) / 4);
	ip_header->dscp_ecn = 0;
	ip_header->length = hostToNet<uint16_t>(sizeof(Ip4Header) + sizeof(UdpHeader) + 5);
	ip_header->identification = hostToNet<uint16_t>(666);
	ip_header->flags_offset = hostToNet<uint16_t>(0);
	ip_header->ttl = kTtl;
	ip_header->protocol = kUdpProtocol;
	ip_header->checksum = 0;
	ip_header->sourceIp = 10 | (0 << 8) | (2 << 16) | (15 << 24);
	ip_header->targetIp = 192 | (168 << 8) | (0 << 16) | (26 << 24);

	Checksum ip_checksum;
	ip_checksum.update(ip_header, sizeof(Ip4Header));
	ip_header->checksum = hostToNet<uint16_t>(ip_checksum.finish());

	auto udp_header = (UdpHeader *)((char *)transmitBuffer + sizeof(VirtHeader) +
			sizeof(EthernetHeader) + sizeof(Ip4Header));

	udp_header->source = hostToNet<uint16_t>(1234);
	udp_header->destination = hostToNet<uint16_t>(1234);
	udp_header->length = hostToNet<uint16_t>(sizeof(UdpHeader) + 5);
	udp_header->checksum = hostToNet<uint16_t>(0);

	auto data = (char *)transmitBuffer + sizeof(VirtHeader) + sizeof(EthernetHeader) +
			sizeof(Ip4Header) + sizeof(UdpHeader);
	data[0] = 'H';
	data[1] = 'a';
	data[2] = 'l';
	data[3] = 'l';
	data[4] = 'o';*/
	
//	udp_header->checksum = ipChecksum(udp_header, sizeof(UdpHeader) + 5);

	/*uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

	auto ethernet = (EthernetHeader *)((char *)transmitBuffer + sizeof(VirtHeader));
	memcpy(ethernet->destAddress, broadcast, 6);
	memcpy(ethernet->sourceAddress, local_mac, 6);
	ethernet->etherType = hostToNet<uint16_t>(kEtherArp);

	auto packet = (ArpPacket *)((char *)transmitBuffer + sizeof(VirtHeader)
			+ sizeof(EthernetHeader));
	packet->hwType = hostToNet<uint16_t>(1);
	packet->protoType = hostToNet<uint16_t>(kEtherIp4);
	packet->hwLength = 6;
	packet->protoLength = 4;
	packet->operation = hostToNet<uint16_t>(1);
	memcpy(packet->senderHw, local_mac, 6);
	packet->senderProto[0] = 10;
	packet->senderProto[1] = 0;
	packet->senderProto[2] = 2;
	packet->senderProto[3] = 15;
	memset(packet->targetHw, 0, 6);
	packet->targetProto[0] = 10;
	packet->targetProto[1] = 0;
	packet->targetProto[2] = 2;
	packet->targetProto[3] = 2; */

	// -----------------------------------------------------------------------------------------

	size_t rx_header_index = receiveQueue.lockDescriptor();
	size_t rx_packet_index = receiveQueue.lockDescriptor();

	// setup a descriptor for the header
	uintptr_t rx_header_physical;
	HEL_CHECK(helPointerPhysical((char *)receiveBuffer, &rx_header_physical));
	
	VirtDescriptor *rx_header_desc = receiveQueue.accessDescriptor(rx_header_index);
	rx_header_desc->address = rx_header_physical;
	rx_header_desc->length = sizeof(VirtHeader);
	rx_header_desc->flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
	rx_header_desc->next = rx_packet_index;

	// setup a descriptor for the packet
	VirtDescriptor *rx_packet_desc = receiveQueue.accessDescriptor(rx_packet_index);
	rx_packet_desc->address = rx_header_physical + sizeof(VirtHeader);
	rx_packet_desc->length = 1514;
	rx_packet_desc->flags = VIRTQ_DESC_F_WRITE;

	receiveQueue.postDescriptor(rx_header_index);
	receiveQueue.notifyDevice();

	while(true) {
		receiveQueue.processInterrupt();
		transmitQueue.processInterrupt();
	}
}

void Device::doInitialize() {
	receiveQueue.setupQueue();
	transmitQueue.setupQueue();

	receiveBuffer = malloc(2048);
	transmitBuffer = malloc(2048);

	// natural alignment to make sure we do not cross page boundaries
	assert((uintptr_t)receiveBuffer % 2048 == 0);
	assert((uintptr_t)transmitBuffer % 2048 == 0);	
}

void Device::retrieveDescriptor(size_t queue_index, size_t desc_index) {
	printf("retrieve(%lu, %lu)\n", queue_index, desc_index);

	if(queue_index == 0) {
		auto packet = (ArpPacket *)((char *)receiveBuffer + sizeof(VirtHeader)
				+ sizeof(EthernetHeader));
		printf("Operation: %d\n", netToHost<uint16_t>(packet->operation));
		printf("Sender MAC: %x:%x:%x:%x:%x:%x\n", packet->senderHw[0], packet->senderHw[1],
				packet->senderHw[2], packet->senderHw[3],
				packet->senderHw[4], packet->senderHw[5]);
		printf("Sender IP: %d.%d.%d.%d\n", packet->senderProto[0], packet->senderProto[1],
				packet->senderProto[2], packet->senderProto[3]);
		printf("Target MAC: %x:%x:%x:%x:%x:%x\n", packet->targetHw[0], packet->targetHw[1],
				packet->targetHw[2], packet->targetHw[3],
				packet->targetHw[4], packet->targetHw[5]);
		printf("Target IP: %d.%d.%d.%d\n", packet->targetProto[0], packet->targetProto[1],
				packet->targetProto[2], packet->targetProto[3]);
	}
}

void Device::afterRetrieve() { }

void Device::onInterrupt(HelError error) { }

} } // namespace virtio::net
