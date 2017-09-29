
#include <assert.h>
#include <iostream>

#include "virtio.hpp"

namespace virtio {

// --------------------------------------------------------
// LegacyPciTransport
// --------------------------------------------------------

namespace {

struct LegacyPciTransport : Transport {
	friend class Queue;

	LegacyPciTransport(protocols::hw::Device hw_device,
			arch::io_space legacy_space, helix::UniqueDescriptor irq);

	uint8_t readConfig8(size_t offset);

	void setupDevice() override;

	helix::BorrowedDescriptor getIrq() override;

	void readIsr() override;

	size_t queryQueue(unsigned int queue_index) override;
	
	void setupQueue(unsigned int queue_index, uintptr_t physical) override;

	void notifyQueue(unsigned int queue_index) override;

private:
	protocols::hw::Device _hwDevice;
	arch::io_space _legacySpace;
	helix::UniqueDescriptor _irq;
};

LegacyPciTransport::LegacyPciTransport(protocols::hw::Device hw_device,
		arch::io_space legacy_space, helix::UniqueDescriptor irq)
: _hwDevice{std::move(hw_device)}, _legacySpace{legacy_space},
		_irq{std::move(irq)} { }

uint8_t LegacyPciTransport::readConfig8(size_t offset) {
	(void)offset;
	throw std::logic_error("Fix virtio::readConfig8()");
//FIXME	return frigg::readIo<uint8_t>(basePort + PCI_L_DEVICE_SPECIFIC + offset);
}

void LegacyPciTransport::setupDevice() {
	// Negotiate features that we want to use.
	uint32_t negotiated = 0;
	uint32_t offered = _legacySpace.load(PCI_L_DEVICE_FEATURES);
	printf("Features %x\n", offered);

	_legacySpace.store(PCI_L_DRIVER_FEATURES, negotiated);

	// Finally set the DRIVER_OK bit to finish the configuration.
	_legacySpace.store(PCI_L_DEVICE_STATUS, _legacySpace.load(PCI_L_DEVICE_STATUS) | DRIVER_OK);
}

helix::BorrowedDescriptor LegacyPciTransport::getIrq() {
	return _irq;
}

void LegacyPciTransport::readIsr() {
	_legacySpace.load(PCI_L_ISR_STATUS);
}

size_t LegacyPciTransport::queryQueue(unsigned int queue_index) {
	_legacySpace.store(PCI_L_QUEUE_SELECT, queue_index);
	return _legacySpace.load(PCI_L_QUEUE_SIZE);

}

void LegacyPciTransport::setupQueue(unsigned int queue_index, uintptr_t physical) {
	_legacySpace.store(PCI_L_QUEUE_SELECT, queue_index);
	_legacySpace.store(PCI_L_QUEUE_ADDRESS, physical >> 12);
}

void LegacyPciTransport::notifyQueue(unsigned int queue_index) {
	_legacySpace.store(PCI_L_QUEUE_NOTIFY, queue_index);
}

} // anonymous namespace

// --------------------------------------------------------
// StandardPciTransport
// --------------------------------------------------------

namespace {

/*
struct StandardPciTransport {

private:
	arch::mem_space _commonSpace;
	arch::mem_space _notifySpace;
	arch::mem_space _isrSpace;
	arch::mem_space _deviceSpace;
};
*/

} // anonymous namespace

// --------------------------------------------------------
// The discover() function.
// --------------------------------------------------------

COFIBER_ROUTINE(async::result<std::unique_ptr<Transport>>,
discover(protocols::hw::Device hw_device), ([hw_device = std::move(hw_device)] () mutable {
	auto info = COFIBER_AWAIT hw_device.getPciInfo();

/*
	for(size_t i = 0; i < info.caps.size(); i++) {
		if(info.caps[i].type != 0x09)
			continue;

		auto subtype = COFIBER_AWAIT hw_device.loadPciCapability(i, 3, 1);
		if(subtype != 1 && subtype != 2 && subtype != 3 && subtype != 4)
			continue;

		auto bir = COFIBER_AWAIT hw_device.loadPciCapability(i, 4, 1);
		auto offset = COFIBER_AWAIT hw_device.loadPciCapability(i, 8, 4);
		auto length = COFIBER_AWAIT hw_device.loadPciCapability(i, 12, 4);
		std::cout << "Subtype: " << subtype << ", BAR index: " << bir
				<< ", offset: " << offset << ", length: " << length << std::endl;
	
		auto bar = COFIBER_AWAIT hw_device.accessBar(bir);
		void *window;
		HEL_CHECK(helMapMemory(bar.getHandle(), kHelNullHandle, nullptr,
				offset, length, kHelMapReadWrite | kHelMapShareAtFork, &window));

		if(subtype == 1) {
			_commonSpace = arch::mem_space{window};
		}else if(subtype == 2) {
			_notifySpace = arch::mem_space{window};
		}else if(subtype == 3) {
			_isrSpace = arch::mem_space{window};
		}else if(subtype == 4) {
			_deviceSpace = arch::mem_space{window};
		}
	}
*/

	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypePort);
	auto bar = COFIBER_AWAIT hw_device.accessBar(0);
	auto irq = COFIBER_AWAIT hw_device.accessIrq();

	HEL_CHECK(helEnableIo(bar.getHandle()));
	
	// Reset the device.
	arch::io_space legacy_space{static_cast<uint16_t>(info.barInfo[0].address)};
	legacy_space.store(PCI_L_DEVICE_STATUS, 0);

	// Set the ACKNOWLEDGE and DRIVER bits.
	// The specification says this should be done in two steps
	legacy_space.store(PCI_L_DEVICE_STATUS, legacy_space.load(PCI_L_DEVICE_STATUS) | ACKNOWLEDGE);
	legacy_space.store(PCI_L_DEVICE_STATUS, legacy_space.load(PCI_L_DEVICE_STATUS) | DRIVER);

	COFIBER_RETURN(std::make_unique<LegacyPciTransport>(std::move(hw_device),
			legacy_space, std::move(irq)));
}))

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

Queue::Queue(Transport *transport, size_t queue_index)
: _transport{transport}, _queueIndex{queue_index}, _queueSize{0}, _table{nullptr},
		_availableRing{nullptr}, _availableExtra{nullptr},
		_usedRing{nullptr}, _usedExtra{nullptr},
		_progressHead{0} { }

void Queue::setupQueue() {
	assert(!_queueSize);

	// Select the queue and determine its size.
	_queueSize = _transport->queryQueue(_queueIndex);
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
	_transport->setupQueue(_queueIndex, physical);
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
		_transport->notifyQueue(_queueIndex);
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

