
#include <assert.h>
#include <stdio.h>

#include "virtio.hpp"

namespace virtio {

// --------------------------------------------------------
// GenericDevice
// --------------------------------------------------------

GenericDevice::GenericDevice()
: space(0) { }

uint8_t GenericDevice::readIsr() {
	return space.load(PCI_L_ISR_STATUS);
}

uint8_t GenericDevice::readConfig8(size_t offset) {
	(void)offset;
	throw std::logic_error("Fix virtio::readConfig8()");
//FIXME	return frigg::readIo<uint8_t>(basePort + PCI_L_DEVICE_SPECIFIC + offset);
}

void GenericDevice::setupDevice(uint16_t base_port, helix::UniqueDescriptor the_interrupt) {
	space = arch::io_space(base_port);
	interrupt = std::move(the_interrupt);

	// Reset the device.
	space.store(PCI_L_DEVICE_STATUS, 0);
	
	// Set the ACKNOWLEDGE and DRIVER bits.
	// The specification says this should be done in two steps
	space.store(PCI_L_DEVICE_STATUS, space.load(PCI_L_DEVICE_STATUS) | ACKNOWLEDGE);
	space.store(PCI_L_DEVICE_STATUS, space.load(PCI_L_DEVICE_STATUS) | DRIVER);
	
	// Negotiate features that we want to use.
	uint32_t negotiated = 0;
	uint32_t offered = space.load(PCI_L_DEVICE_FEATURES);
	printf("Features %x\n", offered);

	space.store(PCI_L_DRIVER_FEATURES, negotiated);

	doInitialize();

	// Finally set the DRIVER_OK bit to finish the configuration.
	space.store(PCI_L_DEVICE_STATUS, space.load(PCI_L_DEVICE_STATUS) | DRIVER_OK);
}

// --------------------------------------------------------
// Handle
// --------------------------------------------------------

Handle::Handle(Queue *queue, size_t table_index)
: _queue{queue}, _tableIndex{table_index} { }

void Handle::setupBuffer(HostToDeviceType, const void *buffer, size_t size) {
	uintptr_t physical;
	HEL_CHECK(helPointerPhysical(const_cast<void *>(buffer), &physical));
	
	auto entry = _queue->accessDescriptor(_tableIndex);
	entry->address = physical;
	entry->length = size;
}

void Handle::setupBuffer(DeviceToHostType, void *buffer, size_t size) {
	uintptr_t physical;
	HEL_CHECK(helPointerPhysical(buffer, &physical));
	
	auto entry = _queue->accessDescriptor(_tableIndex);
	entry->address = physical;
	entry->length = size;
	entry->flags |= VIRTQ_DESC_F_WRITE;
}

void Handle::setupLink(Handle other) {
	auto entry = _queue->accessDescriptor(_tableIndex);
	entry->next = other._tableIndex;
	entry->flags |= VIRTQ_DESC_F_NEXT;
}

// --------------------------------------------------------
// Queue
// --------------------------------------------------------

Queue::Queue(GenericDevice *device, size_t queue_index)
: _device{device}, _queueIndex{queue_index}, _queueSize{0},
		_descriptorPtr(nullptr), _availablePtr(nullptr), _usedPtr(nullptr),
		_progressHead(0) { }

VirtDescriptor *Queue::accessDescriptor(size_t index) {
	return reinterpret_cast<VirtDescriptor *>(_descriptorPtr + index * sizeof(VirtDescriptor));
}

void Queue::setupQueue() {
	assert(!_queueSize);

	// Select the queue and determine its size.
	_device->space.store(PCI_L_QUEUE_SELECT, _queueIndex);
	_queueSize = _device->space.load(PCI_L_QUEUE_SIZE);
	assert(_queueSize > 0);

	for(size_t i = 0; i < _queueSize; i++)
		_descriptorStack.push_back(i);

	// Determine the queue size in bytes.
	size_t available_offset = _queueSize * sizeof(VirtDescriptor);
	size_t used_offset = available_offset + sizeof(VirtAvailableHeader)
			+ _queueSize * sizeof(VirtAvailableRing) + sizeof(VirtAvailableFooter);
	used_offset = (used_offset + 0xFFF) & ~size_t(0xFFF);
	size_t byte_size = used_offset + sizeof(VirtUsedHeader)
			+ _queueSize * sizeof(VirtUsedRing) + sizeof(VirtUsedFooter);

	// Allocate physical memory for the virtq structs.
	assert(byte_size < 0x4000); // FIXME: do not hardcode 0x4000
	HelHandle memory;
	void *pointer;
	HEL_CHECK(helAllocateMemory(0x4000, kHelAllocContinuous, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
			0, 0x4000, kHelMapReadWrite, &pointer));
	HEL_CHECK(helCloseDescriptor(memory));

	_descriptorPtr = (char *)pointer;
	_availablePtr = (char *)pointer + available_offset;
	_usedPtr = (char *)pointer + used_offset;

	// Setup the memory region.
	accessAvailableHeader()->flags = 0;
	accessAvailableHeader()->headIndex = 0;
	accessAvailableFooter()->eventIndex = 0;

	accessUsedHeader()->flags = 0;
	accessUsedHeader()->headIndex = 0;
	accessUsedFooter()->eventIndex = 0;
	
	// Hand the queue to the device.
	uintptr_t physical;
	HEL_CHECK(helPointerPhysical(pointer, &physical));
	_device->space.store(PCI_L_QUEUE_ADDRESS, physical >> 12);
}

size_t Queue::numDescriptors() {
	return _queueSize;
}

size_t Queue::numUnusedDescriptors() {
	return _descriptorStack.size();
}

Handle Queue::obtainDescriptor() {
	assert(!_descriptorStack.empty());
	size_t table_index = _descriptorStack.back();
	_descriptorStack.pop_back();
	memset(accessDescriptor(table_index), 0, sizeof(VirtDescriptor));
	return Handle{this, table_index};
}

void Queue::postDescriptor(Handle descriptor) {
	size_t head = accessAvailableHeader()->headIndex;
	accessAvailableRing(head % _queueSize)->descIndex = descriptor.tableIndex();

	asm volatile ( "" : : : "memory" );
	accessAvailableHeader()->headIndex++;
}

void Queue::notifyDevice() {
	asm volatile ( "" : : : "memory" );
	if(!(accessUsedHeader()->flags & VIRTQ_USED_F_NO_NOTIFY))
		_device->space.store(PCI_L_QUEUE_NOTIFY, _queueIndex);
}

void Queue::processInterrupt() {
	while(true) {
		auto used_head = accessUsedHeader()->headIndex;
		assert(_progressHead <= used_head);
		if(_progressHead == used_head)
			break;
		
		asm volatile ( "" : : : "memory" );
		
		// call the GenericDevice to complete the request
		auto desc_index = accessUsedRing(_progressHead % _queueSize)->descIndex;
		assert(desc_index < _queueSize);
		_device->retrieveDescriptor(_queueIndex, desc_index,
				accessUsedRing(_progressHead % _queueSize)->written);

		// free all descriptors in the descriptor chain
		auto chain = desc_index;
		while(accessDescriptor(chain)->flags & VIRTQ_DESC_F_NEXT) {
			auto successor = accessDescriptor(chain)->next;
			_descriptorStack.push_back(chain);
			chain = successor;
		}
		_descriptorStack.push_back(chain);

		_progressHead++;
	}

	_device->afterRetrieve();
}

} // namespace virtio

