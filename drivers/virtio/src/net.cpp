
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "net.hpp"

extern helx::EventHub eventHub;

enum {
	kEtherIp4 = 0x0800,
	kEtherArp = 0x0806
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

	libnet::testDevice(*this);

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
	/*	auto packet = (ArpPacket *)((char *)receiveBuffer + sizeof(VirtHeader)
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
	*/}
}

void Device::afterRetrieve() { }

void Device::onInterrupt(HelError error) { }

} } // namespace virtio::net
