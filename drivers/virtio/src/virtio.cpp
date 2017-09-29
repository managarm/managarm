
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

void Handle::setupBuffer(HostToDeviceType, arch::dma_buffer_view view) {
	uintptr_t physical;
	HEL_CHECK(helPointerPhysical(view.data(), &physical));
	
	auto descriptor = _queue->_table + _tableIndex;
	descriptor->address.store(physical);
	descriptor->length.store(view.size());
}

void Handle::setupBuffer(DeviceToHostType, arch::dma_buffer_view view) {
	uintptr_t physical;
	HEL_CHECK(helPointerPhysical(view.data(), &physical));
	
	auto descriptor = _queue->_table + _tableIndex;
	descriptor->address.store(physical);
	descriptor->length.store(view.size());
	descriptor->flags.store(descriptor->flags.load() | VIRTQ_DESC_F_WRITE);
}

void Handle::setupLink(Handle other) {
	auto descriptor = _queue->_table + _tableIndex;
	descriptor->next.store(other._tableIndex);
	descriptor->flags.store(descriptor->flags.load() | VIRTQ_DESC_F_NEXT);
}

// --------------------------------------------------------
// Queue
// --------------------------------------------------------

Queue::Queue(GenericDevice *device, size_t queue_index)
: _device{device}, _queueIndex{queue_index}, _queueSize{0}, _table{nullptr},
		_availableRing{nullptr}, _availableExtra{nullptr},
		_usedRing{nullptr}, _usedExtra{nullptr},
		_progressHead{0} { }

void Queue::setupQueue() {
	assert(!_queueSize);

	// Select the queue and determine its size.
	_device->space.store(PCI_L_QUEUE_SELECT, _queueIndex);
	_queueSize = _device->space.load(PCI_L_QUEUE_SIZE);
	assert(_queueSize > 0);

	// TODO: Ensure that the queue size is indeed a power of 2.

	for(size_t i = 0; i < _queueSize; i++)
		_descriptorStack.push_back(i);
	_activeRequests.resize(_queueSize);

	// Determine the queue size in bytes.
	size_t available_offset = _queueSize * sizeof(spec::Descriptor);
	size_t available_extra_offset = available_offset + sizeof(spec::AvailableRing)
			+ _queueSize * sizeof(spec::AvailableRing::Element);;
	size_t used_offset = available_extra_offset + sizeof(spec::AvailableExtra);
	used_offset = (used_offset + 0xFFF) & ~size_t(0xFFF);
	size_t used_extra_offset = used_offset + sizeof(spec::UsedRing)
			+ _queueSize * sizeof(spec::UsedRing::Element);;
	size_t end_offset = used_extra_offset + sizeof(spec::UsedExtra);

	// Allocate physical memory for the virtq structs.
	assert(end_offset < 0x4000); // FIXME: do not hardcode 0x4000
	HelHandle memory;
	void *window;
	HEL_CHECK(helAllocateMemory(0x4000, kHelAllocContinuous, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
			0, 0x4000, kHelMapReadWrite, &window));
	HEL_CHECK(helCloseDescriptor(memory));

	_table = reinterpret_cast<spec::Descriptor *>((char *)window);
	_availableRing = new ((char *)window + available_offset) spec::AvailableRing;
	_availableExtra = new ((char *)window + available_extra_offset) spec::AvailableExtra;
	_usedRing = new ((char *)window + used_offset) spec::UsedRing;
	_usedExtra = new ((char *)window + used_extra_offset) spec::UsedExtra;

	// Setup the memory region.
	_availableRing->flags.store(0);
	_availableRing->headIndex.store(0);
	_availableExtra->eventIndex.store(0);

	_usedRing->flags.store(0);
	_usedRing->headIndex.store(0);
	_usedExtra->eventIndex.store(0);
	
	// Hand the queue to the device.
	uintptr_t physical;
	HEL_CHECK(helPointerPhysical(window, &physical));
	_device->space.store(PCI_L_QUEUE_ADDRESS, physical >> 12);
}

size_t Queue::numDescriptors() {
	return _queueSize;
}

COFIBER_ROUTINE(async::result<Handle>, Queue::obtainDescriptor(), ([=] {
	while(true) {
		if(_descriptorStack.empty()) {
			COFIBER_AWAIT _descriptorDoorbell.async_wait();
			continue;
		}

		size_t table_index = _descriptorStack.back();
		_descriptorStack.pop_back();
		
		auto descriptor = _table + table_index;
		descriptor->address.store(0);
		descriptor->length.store(0);
		descriptor->flags.store(0);

		COFIBER_RETURN((Handle{this, table_index}));
	}
}))

void Queue::postDescriptor(Handle handle, Request *request,
		void (*complete)(Request *)) {
	request->complete = complete;

	assert(request);
	assert(!_activeRequests[handle.tableIndex()]);
	_activeRequests[handle.tableIndex()] = request;

	auto enqueue_head = _availableRing->headIndex.load();
	auto ring_index = enqueue_head & (_queueSize - 1);
	_availableRing->elements[ring_index].tableIndex.store(handle.tableIndex());

	asm volatile ( "" : : : "memory" );
	_availableRing->headIndex.store(enqueue_head + 1);
}

void Queue::notifyDevice() {
	asm volatile ( "" : : : "memory" );
	if(!(_usedRing->flags.load() & VIRTQ_USED_F_NO_NOTIFY))
		_device->space.store(PCI_L_QUEUE_NOTIFY, _queueIndex);
}

void Queue::processInterrupt() {
	while(true) {
		auto used_head = _usedRing->headIndex.load();
		// TODO: I think this assertion is incorrect once we issue more than 2^16 requests.
		assert(_progressHead <= used_head);
		if(_progressHead == used_head)
			break;
		
		asm volatile ( "" : : : "memory" );

		auto ring_index = _progressHead & (_queueSize - 1);
		auto table_index = _usedRing->elements[ring_index].tableIndex.load();
		assert(table_index < _queueSize);

		// Dequeue the Request object.
		auto request = _activeRequests[table_index];
		assert(request);
		_activeRequests[table_index] = nullptr;

		// Free all descriptors in the descriptor chain.
		auto chain_index = table_index;
		while(_table[chain_index].flags.load() & VIRTQ_DESC_F_NEXT) {
			auto successor = _table[chain_index].next.load();
			_descriptorStack.push_back(chain_index);
			chain_index = successor;
		}
		_descriptorStack.push_back(chain_index);
		_descriptorDoorbell.ring();

		// Call the completion handler.
		request->complete(request);

		_progressHead++;
	}
}

} // namespace virtio

