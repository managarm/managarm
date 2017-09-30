
#include <assert.h>
#include <iostream>
#include <optional>

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

	void finalizeFeatures() override;
	void runDevice() override;

	helix::BorrowedDescriptor getIrq() override;
	void readIsr() override;

	QueueInfo queryQueueInfo(unsigned int queue_index) override;
	void setupQueue(unsigned int queue_index, uintptr_t table_physical,
			uintptr_t available_physical, uintptr_t used_physical) override;
	void notifyDevice(unsigned int queue_index, ptrdiff_t notify_offset) override;

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

void LegacyPciTransport::finalizeFeatures() {
	// Does nothing for now.
}

void LegacyPciTransport::runDevice() {
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

QueueInfo LegacyPciTransport::queryQueueInfo(unsigned int queue_index) {
	_legacySpace.store(PCI_L_QUEUE_SELECT, queue_index);
	auto queue_size = _legacySpace.load(PCI_L_QUEUE_SIZE);
	return {queue_size, 0};

}

void LegacyPciTransport::setupQueue(unsigned int queue_index, uintptr_t table_physical,
			uintptr_t available_physical, uintptr_t used_physical) {
	(void)available_physical;
	(void)used_physical;
	_legacySpace.store(PCI_L_QUEUE_SELECT, queue_index);
	_legacySpace.store(PCI_L_QUEUE_ADDRESS, table_physical >> 12);
}

void LegacyPciTransport::notifyDevice(unsigned int queue_index, ptrdiff_t notify_offset) {
	assert(!notify_offset);
	_legacySpace.store(PCI_L_QUEUE_NOTIFY, queue_index);
}

} // anonymous namespace

// --------------------------------------------------------
// StandardPciTransport
// --------------------------------------------------------

namespace {

struct StandardPciTransport : Transport {
	StandardPciTransport(protocols::hw::Device hw_device,
			arch::mem_space common_space, arch::mem_space notify_space,
			arch::mem_space isr_space, arch::mem_space device_space,
			unsigned int notify_multiplier, helix::UniqueDescriptor irq);

	bool checkDeviceFeature(unsigned int feature);
	void acknowledgeDriverFeature(unsigned int feature);

	void finalizeFeatures() override;
	void runDevice() override;

	helix::BorrowedDescriptor getIrq() override;
	void readIsr() override;

	QueueInfo queryQueueInfo(unsigned int queue_index) override;
	void setupQueue(unsigned int queue_index, uintptr_t table_physical,
			uintptr_t available_physical, uintptr_t used_physical) override;
	void notifyDevice(unsigned int queue_index, ptrdiff_t notify_offset) override;

private:
	protocols::hw::Device _hwDevice;
	arch::mem_space _commonSpace;
	arch::mem_space _notifySpace;
	arch::mem_space _isrSpace;
	arch::mem_space _deviceSpace;
	unsigned int _notifyMultiplier;
	helix::UniqueDescriptor _irq;
};

StandardPciTransport::StandardPciTransport(protocols::hw::Device hw_device,
		arch::mem_space common_space, arch::mem_space notify_space,
		arch::mem_space isr_space, arch::mem_space device_space,
		unsigned int notify_multiplier, helix::UniqueDescriptor irq)
: _hwDevice{std::move(hw_device)}, _commonSpace{common_space}, _notifySpace{notify_space},
		_isrSpace{isr_space}, _deviceSpace{device_space},
		_notifyMultiplier{notify_multiplier}, _irq{std::move(irq)} { }

/*
uint8_t StandardPciTransport::readConfig8(size_t offset) {
	(void)offset;
	throw std::logic_error("Fix virtio::readConfig8()");
}
*/

bool StandardPciTransport::checkDeviceFeature(unsigned int feature) {
	_commonSpace.store(PCI_DEVICE_FEATURE_SELECT, feature >> 5);
	return _commonSpace.load(PCI_DEVICE_FEATURE_WINDOW) & (uint32_t(1) << (feature & 31));
}

void StandardPciTransport::acknowledgeDriverFeature(unsigned int feature) {
	auto bit = uint32_t(1) << (feature & 31);
	_commonSpace.store(PCI_DRIVER_FEATURE_SELECT, feature >> 5);
	auto current = _commonSpace.load(PCI_DRIVER_FEATURE_WINDOW);
	_commonSpace.store(PCI_DRIVER_FEATURE_WINDOW, current | bit);
}

void StandardPciTransport::finalizeFeatures() {
	assert(checkDeviceFeature(32));
	acknowledgeDriverFeature(32);

	_commonSpace.store(PCI_DEVICE_STATUS, _commonSpace.load(PCI_DEVICE_STATUS) | FEATURES_OK);
	auto confirm = _commonSpace.load(PCI_DEVICE_STATUS);
	assert(confirm & FEATURES_OK);
}

void StandardPciTransport::runDevice() {
	// Finally set the DRIVER_OK bit to finish the configuration.
	_commonSpace.store(PCI_DEVICE_STATUS, _commonSpace.load(PCI_DEVICE_STATUS) | DRIVER_OK);
}

helix::BorrowedDescriptor StandardPciTransport::getIrq() {
	return _irq;
}

void StandardPciTransport::readIsr() {
	_isrSpace.load(PCI_ISR);
}

QueueInfo StandardPciTransport::queryQueueInfo(unsigned int queue_index) {
	_commonSpace.store(PCI_QUEUE_SELECT, queue_index);
	auto queue_size = _commonSpace.load(PCI_QUEUE_SIZE);
	auto notify_index = _commonSpace.load(PCI_QUEUE_NOTIFY);
	return {queue_size, notify_index * _notifyMultiplier};
}

void StandardPciTransport::setupQueue(unsigned int queue_index,
		uintptr_t table_physical, uintptr_t available_physical, uintptr_t used_physical) {
	assert(!(table_physical & 15));
	assert(!(available_physical & 1));
	assert(!(used_physical & 3));
	_commonSpace.store(PCI_QUEUE_SELECT, queue_index);
	_commonSpace.store(PCI_QUEUE_TABLE[0], table_physical);
	_commonSpace.store(PCI_QUEUE_TABLE[1], table_physical >> 32);
	_commonSpace.store(PCI_QUEUE_AVAILABLE[0], available_physical);
	_commonSpace.store(PCI_QUEUE_AVAILABLE[1], available_physical >> 32);
	_commonSpace.store(PCI_QUEUE_USED[0], used_physical);
	_commonSpace.store(PCI_QUEUE_USED[1], used_physical >> 32);
	_commonSpace.store(PCI_QUEUE_ENABLE, 1);
}

void StandardPciTransport::notifyDevice(unsigned int queue_index, ptrdiff_t notify_offset) {
	_notifySpace.store(arch::scalar_register<uint16_t>(notify_offset), queue_index);
}

} // anonymous namespace

// --------------------------------------------------------
// The discover() function.
// --------------------------------------------------------

COFIBER_ROUTINE(async::result<std::unique_ptr<Transport>>,
discover(protocols::hw::Device hw_device), ([hw_device = std::move(hw_device)] () mutable {
	auto info = COFIBER_AWAIT hw_device.getPciInfo();
	auto irq = COFIBER_AWAIT hw_device.accessIrq();

	if(true) {
		std::optional<arch::mem_space> common_space;
		std::optional<arch::mem_space> notify_space;
		std::optional<arch::mem_space> isr_space;
		std::optional<arch::mem_space> device_space;
		unsigned int notify_multiplier = 0;

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
				common_space = arch::mem_space{window};
			}else if(subtype == 2) {
				notify_space = arch::mem_space{window};
				notify_multiplier = COFIBER_AWAIT hw_device.loadPciCapability(i, 16, 4);
			}else if(subtype == 3) {
				isr_space = arch::mem_space{window};
			}else if(subtype == 4) {
				device_space = arch::mem_space{window};
			}
		}
		
		if(common_space && notify_space && isr_space && device_space) {
			// Reset the device.
			common_space->store(PCI_DEVICE_STATUS, 0);

			// Set the ACKNOWLEDGE and DRIVER bits.
			// The specification says this should be done in two steps
			common_space->store(PCI_DEVICE_STATUS,
					common_space->load(PCI_DEVICE_STATUS) | ACKNOWLEDGE);
			common_space->store(PCI_DEVICE_STATUS,
					common_space->load(PCI_DEVICE_STATUS) | DRIVER);

			COFIBER_RETURN(std::make_unique<StandardPciTransport>(std::move(hw_device),
					*common_space, *notify_space, *isr_space, *device_space,
					notify_multiplier, std::move(irq)));
		}
	}

	if(false) {
		assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypePort);
		auto bar = COFIBER_AWAIT hw_device.accessBar(0);
		HEL_CHECK(helEnableIo(bar.getHandle()));
		
		// Reset the device.
		arch::io_space legacy_space{static_cast<uint16_t>(info.barInfo[0].address)};
		legacy_space.store(PCI_L_DEVICE_STATUS, 0);

		// Set the ACKNOWLEDGE and DRIVER bits.
		// The specification says this should be done in two steps
		legacy_space.store(PCI_L_DEVICE_STATUS,
				legacy_space.load(PCI_L_DEVICE_STATUS) | ACKNOWLEDGE);
		legacy_space.store(PCI_L_DEVICE_STATUS,
				legacy_space.load(PCI_L_DEVICE_STATUS) | DRIVER);

		COFIBER_RETURN(std::make_unique<LegacyPciTransport>(std::move(hw_device),
				legacy_space, std::move(irq)));
	}

	throw std::runtime_error("Cannot construct a suitable virtio::Transport");
}))

// --------------------------------------------------------
// Handle
// --------------------------------------------------------

Handle::Handle(Queue *queue, size_t table_index)
: _queue{queue}, _tableIndex{table_index} { }

void Handle::setupBuffer(HostToDeviceType, arch::dma_buffer_view view) {
	assert(view.size());

	uintptr_t physical;
	HEL_CHECK(helPointerPhysical(view.data(), &physical));
	
	auto descriptor = _queue->_table + _tableIndex;
	descriptor->address.store(physical);
	descriptor->length.store(view.size());
}

void Handle::setupBuffer(DeviceToHostType, arch::dma_buffer_view view) {
	assert(view.size());

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
	auto info = _transport->queryQueueInfo(_queueIndex);
	_queueSize = info.queueSize;
	_notifyOffset = info.notifyOffset;
	assert(_queueSize > 0);

	// TODO: Ensure that the queue size is indeed a power of 2.

	for(size_t i = 0; i < _queueSize; i++)
		_descriptorStack.push_back(i);
	_activeRequests.resize(_queueSize);

	// Determine the queue size in bytes.
	size_t available_offset = _queueSize * sizeof(spec::Descriptor);
//	available_offset = (available_offset + 0xFFF) & ~size_t(0xFFF);
	size_t available_extra_offset = available_offset + sizeof(spec::AvailableRing)
			+ _queueSize * sizeof(spec::AvailableRing::Element);;
	size_t used_offset = available_extra_offset + sizeof(spec::AvailableExtra);
	used_offset = (used_offset + 0xFFF) & ~size_t(0xFFF);
//	used_offset = (used_offset + 3) & ~size_t(3);
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
	for(size_t i = 0; i < _queueSize; i++)
		new (_table + i) spec::Descriptor;
	_availableRing = new ((char *)window + available_offset) spec::AvailableRing;
	_availableExtra = new ((char *)window + available_extra_offset) spec::AvailableExtra;
	_usedRing = new ((char *)window + used_offset) spec::UsedRing;
	_usedExtra = new ((char *)window + used_extra_offset) spec::UsedExtra;

	// Setup the memory region.
	_availableRing->flags.store(0);
	_availableRing->headIndex.store(0);
	for(size_t i = 0; i < _queueSize; i++)
		_availableRing->elements[i].tableIndex.store(0xFFFF);
	_availableExtra->eventIndex.store(0);

	_usedRing->flags.store(0);
	_usedRing->headIndex.store(0);
	for(size_t i = 0; i < _queueSize; i++)
		_usedRing->elements[i].tableIndex.store(0xFFFF);
	_usedExtra->eventIndex.store(0);
	
	// Hand the queue to the device.
	uintptr_t table_physical, available_physical, used_physical;
	HEL_CHECK(helPointerPhysical(_table, &table_physical));
	HEL_CHECK(helPointerPhysical(_availableRing, &available_physical));
	HEL_CHECK(helPointerPhysical(_usedRing, &used_physical));
	_transport->setupQueue(_queueIndex, table_physical, available_physical, used_physical);
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
		_transport->notifyDevice(_queueIndex, _notifyOffset);
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

