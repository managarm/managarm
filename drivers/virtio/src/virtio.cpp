
#include <assert.h>
#include <stdio.h>

#include <frigg/arch_x86/machine.hpp>
#include <helx.hpp>

#include "virtio.hpp"

namespace virtio {

// --------------------------------------------------------
// GenericDevice
// --------------------------------------------------------

GenericDevice::GenericDevice()
: basePort(0) { }

uint16_t GenericDevice::readIsr() {
	return frigg::readIo<uint16_t>(basePort + PCI_L_ISR_STATUS);
}

uint8_t GenericDevice::readConfig8(size_t offset) {
	return frigg::readIo<uint16_t>(basePort + PCI_L_DEVICE_SPECIFIC + offset);
}

void GenericDevice::setupDevice(uint16_t base_port) {
	basePort = base_port;

	// reset the device
	frigg::writeIo<uint8_t>(basePort + PCI_L_DEVICE_STATUS, 0);
	
	// set the ACKNOWLEDGE and DRIVER bits
	// the specification says this should be done in two steps
	frigg::writeIo<uint8_t>(basePort + PCI_L_DEVICE_STATUS,
			frigg::readIo<uint8_t>(basePort + PCI_L_DEVICE_STATUS) | ACKNOWLEDGE);
	frigg::writeIo<uint8_t>(basePort + PCI_L_DEVICE_STATUS,
			frigg::readIo<uint8_t>(basePort + PCI_L_DEVICE_STATUS) | DRIVER);
	
	// negotiate features we want to use
	uint32_t negotiated = 0;
	uint32_t offered = frigg::readIo<uint32_t>(basePort + PCI_L_DEVICE_FEATURES);
	printf("Features %x\n", offered);

	frigg::writeIo<uint32_t>(basePort + PCI_L_DRIVER_FEATURES, negotiated);

	doInitialize();

	// finally set the DRIVER_OK bit to finish the configuration
	frigg::writeIo<uint8_t>(basePort + PCI_L_DEVICE_STATUS,
			frigg::readIo<uint8_t>(basePort + PCI_L_DEVICE_STATUS) | DRIVER_OK);
}

// --------------------------------------------------------
// Queue
// --------------------------------------------------------

Queue::Queue(GenericDevice &device, size_t queue_index)
: device(device), queueIndex(queue_index), queueSize(0),
		descriptorPtr(nullptr), availPtr(nullptr), usedPtr(nullptr),
		progressHead(0) { }

VirtDescriptor *Queue::accessDescriptor(size_t index) {
	return reinterpret_cast<VirtDescriptor *>(descriptorPtr + index * sizeof(VirtDescriptor));
}

void Queue::setupQueue() {
	assert(!queueSize);

	// select the queue and determine it's size
	frigg::writeIo<uint16_t>(device.basePort + PCI_L_QUEUE_SELECT, queueIndex);
	queueSize = frigg::readIo<uint16_t>(device.basePort + PCI_L_QUEUE_SIZE);
	assert(queueSize > 0);

	for(size_t i = 0; i < queueSize; i++)
		descriptorStack.push_back(i);

	// determine the queue size in bytes
	size_t avail_offset = queueSize * sizeof(VirtDescriptor);
	size_t used_offset = avail_offset + sizeof(VirtAvailHeader)
			+ queueSize * sizeof(VirtAvailRing) + sizeof(VirtAvailFooter);
	if(used_offset % kQueueAlign)
		used_offset += kQueueAlign - (used_offset % kQueueAlign);
	size_t byte_size = used_offset + sizeof(VirtUsedHeader)
			+ queueSize * sizeof(VirtUsedRing) + sizeof(VirtUsedFooter);

	// allocate physical memory for the virtqueue structs
	assert(byte_size < 0x4000); // FIXME: do not hardcode 0x4000
	HelHandle memory;
	void *pointer;
	HEL_CHECK(helAllocateMemory(0x4000, kHelAllocContinuous, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
			0, 0x4000, kHelMapReadWrite, &pointer));
	HEL_CHECK(helCloseDescriptor(memory));

	descriptorPtr = (char *)pointer;
	availPtr = (char *)pointer + avail_offset;
	usedPtr = (char *)pointer + used_offset;

	// setup the memory region
	accessAvailHeader()->flags = 0;
	accessAvailHeader()->headIndex = 0;
	accessAvailFooter()->eventIndex = 0;

	accessUsedHeader()->flags = 0;
	accessUsedHeader()->headIndex = 0;
	accessUsedFooter()->eventIndex = 0;
	
	// hand the queue to the device
	uintptr_t physical;
	HEL_CHECK(helPointerPhysical(pointer, &physical));

	frigg::writeIo<uint32_t>(device.basePort + PCI_L_QUEUE_ADDRESS, physical / 0x1000);
}

size_t Queue::getSize() {
	return queueSize;
}

size_t Queue::numLockable() {
	return descriptorStack.size();
}

size_t Queue::lockDescriptor() {
	assert(!descriptorStack.empty());
	size_t index = descriptorStack.back();
	descriptorStack.pop_back();
	return index;
}

void Queue::postDescriptor(size_t desc_index) {
	size_t head = accessAvailHeader()->headIndex;
	accessAvailRing(head % queueSize)->descIndex = desc_index;

	asm volatile ( "" : : : "memory" );
	accessAvailHeader()->headIndex++;
}

void Queue::notifyDevice() {
	asm volatile ( "" : : : "memory" );
	if(!(accessUsedHeader()->flags & VIRTQ_USED_F_NO_NOTIFY))
		frigg::writeIo<uint16_t>(device.basePort + PCI_L_QUEUE_NOTIFY, queueIndex);
}

void Queue::processInterrupt() {
	while(true) {
		auto used_head = accessUsedHeader()->headIndex;
		assert(progressHead <= used_head);
		if(progressHead == used_head)
			break;
		
		asm volatile ( "" : : : "memory" );
		
		// call the GenericDevice to complete the request
		auto desc_index = accessUsedRing(progressHead % queueSize)->descIndex;
		assert(desc_index < queueSize);
		device.retrieveDescriptor(queueIndex, desc_index,
				accessUsedRing(progressHead % queueSize)->written);

		// free all descriptors in the descriptor chain
		auto chain = desc_index;
		while(accessDescriptor(chain)->flags & VIRTQ_DESC_F_NEXT) {
			auto successor = accessDescriptor(chain)->next;
			descriptorStack.push_back(chain);
			chain = successor;
		}
		descriptorStack.push_back(chain);

		progressHead++;
	}

	device.afterRetrieve();
}

} // namespace virtio

