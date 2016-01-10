
#include <stdio.h>
#include <stdlib.h>
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
uint16_t netToHost(uint16_t value) {
	return __builtin_bswap16(value);
}

extern helx::EventHub eventHub;

enum {
	kEtherIp4 = 0x0800,
	kEtherArp = 0x0806
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

namespace virtio {
namespace net {

void *receiveBuffer;
void *transmitBuffer;

Device::Device()
		: receiveQueue(*this, 0), transmitQueue(*this, 1) { }

void Device::testDevice() {
	size_t tx_header_index = transmitQueue.lockDescriptor();
	size_t tx_packet_index = transmitQueue.lockDescriptor();

	auto header = (VirtHeader *)transmitBuffer;
	header->flags = 0;
	header->gsoType = 0;
	header->hdrLen = 0;
	header->gsoSize = 0;
	header->csumStart = 0;
	header->csumOffset = 0;

	uint8_t mac[6];
	for(size_t i = 0; i < 6; i++)
		mac[i] = readConfig8(i);
	
	printf("MAC: %x:%x:%x:%x:%x:%x\n", mac[0], mac[1], mac[2],
			mac[3], mac[4], mac[5]);

	uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

	auto ethernet = (EthernetHeader *)((char *)transmitBuffer + sizeof(VirtHeader));
	memcpy(ethernet->destAddress, broadcast, 6);
	memcpy(ethernet->sourceAddress, mac, 6);
	ethernet->etherType = hostToNet<uint16_t>(kEtherArp);

	auto packet = (ArpPacket *)((char *)transmitBuffer + sizeof(VirtHeader)
			+ sizeof(EthernetHeader));
	packet->hwType = hostToNet<uint16_t>(1);
	packet->protoType = hostToNet<uint16_t>(kEtherIp4);
	packet->hwLength = 6;
	packet->protoLength = 4;
	packet->operation = hostToNet<uint16_t>(1);
	memcpy(packet->senderHw, mac, 6);
	packet->senderProto[0] = 10;
	packet->senderProto[1] = 0;
	packet->senderProto[2] = 2;
	packet->senderProto[3] = 15;
	memset(packet->targetHw, 0, 6);
	packet->targetProto[0] = 10;
	packet->targetProto[1] = 0;
	packet->targetProto[2] = 2;
	packet->targetProto[3] = 3;

	// setup a descriptor for the header
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
	tx_packet_desc->length = 1514;
	tx_packet_desc->flags = 0;

	transmitQueue.postDescriptor(tx_header_index);
	transmitQueue.notifyDevice();

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
