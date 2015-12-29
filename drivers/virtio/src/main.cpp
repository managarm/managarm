
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <vector>
#include <memory>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include <frigg/atomic.hpp>
#include <frigg/arch_x86/machine.hpp>

#include <bragi/mbus.hpp>
#include <hw.pb.h>
#include <libfs.hpp>

helx::EventHub eventHub = helx::EventHub::create();
bragi_mbus::Connection mbusConnection(eventHub);

// --------------------------------------------------------
// VirtIO data structures and constants
// --------------------------------------------------------

enum {
	PCI_L_DEVICE_FEATURES = 0,
	PCI_L_DRIVER_FEATURES = 4,
	PCI_L_QUEUE_ADDRESS = 8,
	PCI_L_QUEUE_SIZE = 12,
	PCI_L_QUEUE_SELECT = 14,
	PCI_L_QUEUE_NOTIFY = 16,
	PCI_L_DEVICE_STATUS = 18,
	PCI_L_ISR_STATUS = 19
};

// bits of the device status register
enum {
	ACKNOWLEDGE = 1,
	DRIVER = 2,
	DRIVER_OK = 4
};

struct VirtDescriptor {
	uint64_t address;
	uint32_t length;
	uint16_t flags;
	uint16_t next;
};

// bits of the VirtDescriptor::flags field
enum {
	VIRTQ_DESC_F_NEXT = 1, // descriptor is part of a chain
	VIRTQ_DESC_F_WRITE = 2 // buffer is written by device
};

struct VirtAvailHeader {
	uint16_t flags;
	uint16_t headIndex;
};
struct VirtAvailRing {
	uint16_t descIndex;
};
struct VirtAvailFooter {
	uint16_t eventIndex;
};

struct VirtUsedHeader {
	uint16_t flags;
	uint16_t headIndex;
};
struct VirtUsedRing {
	uint32_t descIndex;
	uint32_t written;
};
struct VirtUsedFooter {
	uint16_t eventIndex;
};

uint16_t basePort = 0xC040; // FIXME: read from PCI device

// --------------------------------------------------------

// values for the VirtRequest::type field
enum {
	VIRTIO_BLK_T_IN = 0,
	VIRTIO_BLK_T_OUT = 1
};

constexpr int DATA_SIZE = 512;

struct VirtRequest {
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
	uint8_t data[DATA_SIZE];
	uint8_t status;
};

// --------------------------------------------------------

struct UserRequest {
	void *buffer;
	frigg::CallbackPtr<void()> callback;
	size_t countdown;
};

struct RequestSlot {
	enum SlotType {
		kSlotNone,
		kSlotRead
	};

	SlotType slotType;
	UserRequest *userRequest;
	size_t bufferIndex;
	size_t part;
};

struct PseudoDescriptor {
	uintptr_t address;
	size_t length;
	uint16_t flags;
};

// --------------------------------------------------------
// Queue
// --------------------------------------------------------

char *requestSpace;
std::vector<size_t> bufferStack;

struct Queue {
	Queue(size_t queue_index);

	void setupQueue(uintptr_t physical);

	void postRequest(PseudoDescriptor *pseudo, size_t count, RequestSlot slot);

	void notifyDevice();

	void process();

private:
	static constexpr size_t kQueueAlign = 0x1000;

	auto accessDesc(size_t index) {
		return reinterpret_cast<VirtDescriptor *>(descPtr + index * sizeof(VirtDescriptor));
	}

	auto accessAvailHeader() {
		return reinterpret_cast<VirtAvailHeader *>(availPtr);
	}
	auto accessAvailRing(size_t index) {
		return reinterpret_cast<VirtAvailRing *>(availPtr + sizeof(VirtAvailHeader)
				+ index * sizeof(VirtAvailRing));
	}
	auto accessAvailFooter() {
		return reinterpret_cast<VirtAvailFooter *>(availPtr + sizeof(VirtAvailHeader)
				+ queueSize * sizeof(VirtAvailRing));
	}

	auto accessUsedHeader() {
		return reinterpret_cast<VirtUsedHeader *>(usedPtr);
	}
	auto accessUsedRing(size_t index) {
		return reinterpret_cast<VirtUsedRing *>(usedPtr + sizeof(VirtUsedHeader)
				+ index * sizeof(VirtUsedRing));
	}
	auto accessUsedFooter() {
		return reinterpret_cast<VirtUsedFooter *>(usedPtr + sizeof(VirtUsedHeader)
				+ queueSize * sizeof(VirtUsedRing));
	}

	// index of this queue relativate to its owning device
	size_t queueIndex;
	
	// number of descriptors in this queue
	size_t queueSize;
	
	// pointers to different data structures of this queue
	char *descPtr, *availPtr, *usedPtr;

	// keeps track of unused descriptor indices
	std::vector<uint16_t> descriptorStack;
	
	// associates queue descriptors with RequestSlot objects
	std::vector<RequestSlot> slots;

	// keeps track of which entries in the used ring have already been processed
	uint16_t progressHead;
};

Queue::Queue(size_t queue_index)
: queueIndex(queue_index), queueSize(0),
		descPtr(nullptr), availPtr(nullptr), usedPtr(nullptr),
		progressHead(0) { }

void Queue::setupQueue(uintptr_t physical) {
	assert(!queueSize);

	// select the queue and determine it's size
	frigg::writeIo<uint16_t>(basePort + PCI_L_QUEUE_SELECT, 0);
	queueSize = frigg::readIo<uint16_t>(basePort + PCI_L_QUEUE_SIZE);
	assert(queueSize > 0);

	for(size_t i = 0; i < queueSize; i++)
		descriptorStack.push_back(i);
	slots.resize(queueSize);

	// determine the queue size in bytes
	size_t avail_offset = queueSize * sizeof(VirtDescriptor);
	size_t used_offset = avail_offset + sizeof(VirtAvailHeader)
			+ queueSize * sizeof(VirtAvailRing) + sizeof(VirtAvailFooter);
	if(used_offset % kQueueAlign)
		used_offset += kQueueAlign - (used_offset % kQueueAlign);
	size_t byte_size = used_offset + sizeof(VirtUsedHeader)
			+ queueSize * sizeof(VirtUsedRing) + sizeof(VirtUsedFooter);

	size_t memory_size = byte_size;
	if(memory_size % 0x1000)
		memory_size += 0x1000 - (memory_size % 0x1000);
	
	// FIXME: allocate memory instead of using a fixed address
	HelHandle memory;
	void *pointer;
	HEL_CHECK(helAccessPhysical(physical, memory_size, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
			memory_size, kHelMapReadWrite, &pointer));
	HEL_CHECK(helCloseDescriptor(memory));

	descPtr = (char *)pointer;
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
	frigg::writeIo<uint32_t>(basePort + PCI_L_QUEUE_ADDRESS, physical / 0x1000);
}

void Queue::postRequest(PseudoDescriptor *pseudo, size_t count, RequestSlot slot) {
	assert(count > 0);

	assert(!descriptorStack.empty());
	uint16_t first_index = descriptorStack.back();
	descriptorStack.pop_back();

	uint16_t current_index = first_index;
	for(size_t i = 0; i < count; i++) {
		assert(!(pseudo[i].flags & VIRTQ_DESC_F_NEXT));
		accessDesc(current_index)->address = pseudo[i].address;
		accessDesc(current_index)->length = pseudo[i].length;
		accessDesc(current_index)->flags = pseudo[i].flags;
		
		// if this is not the last descriptor we have to chain another one
		if(i + 1 < count) {
			assert(!descriptorStack.empty());
			uint16_t chain_index = descriptorStack.back();
			descriptorStack.pop_back();
			
			accessDesc(current_index)->flags |= VIRTQ_DESC_F_NEXT;
			accessDesc(current_index)->next = chain_index;
			current_index = chain_index;
		}
	}

	slots[first_index] = slot;
	
	size_t head = accessAvailHeader()->headIndex;
	accessAvailRing(head % queueSize)->descIndex = first_index;
	asm volatile ( "" : : : "memory" );
	accessAvailHeader()->headIndex++;
}

void Queue::notifyDevice() {
	asm volatile ( "" : : : "memory" );
	frigg::writeIo<uint16_t>(basePort + PCI_L_QUEUE_NOTIFY, queueIndex);
}

void Queue::process() {
	while(true) {
		auto used_head = accessUsedHeader()->headIndex;
		assert(progressHead <= used_head);
		if(progressHead == used_head)
			break;
		
		asm volatile ( "" : : : "memory" );
		
		auto desc_index = accessUsedRing(progressHead % queueSize)->descIndex;
		assert(desc_index < queueSize);
		auto slot = &slots[desc_index];
		assert(slot->slotType == RequestSlot::kSlotRead);
		auto user_request = slot->userRequest;

		//FIXME assert(virt_request->status == 0);
		memcpy((char *)user_request->buffer + slot->part * 512,
				requestSpace + 0x10 + slot->bufferIndex * 0x400, 512);
		bufferStack.push_back(slot->bufferIndex);
		
		// reset the request type (for debugging purposes)
		slot->slotType = RequestSlot::kSlotNone;

		// invoke the user callback if all partial requests are completed
		assert(user_request->countdown > 0);
		user_request->countdown--;
		if(!user_request->countdown) {
			user_request->callback();
			delete user_request;
		}

		// free all descriptors in the descriptor chain
		auto chain = desc_index;
		while(accessDesc(chain)->flags & VIRTQ_DESC_F_NEXT) {
			auto successor = accessDesc(chain)->next;
			descriptorStack.push_back(chain);
			chain = successor;
		}
		descriptorStack.push_back(chain);
		progressHead++;
	}
}

Queue queue0(0);

// --------------------------------------------------------

struct Device : public libfs::BlockDevice {
	Device();

	void readSectors(uint64_t sector, void *buffer, size_t num_sectors,
			frigg::CallbackPtr<void()> callback) override;
};

Device::Device()
: libfs::BlockDevice(512) { }

void Device::readSectors(uint64_t sector, void *buffer, size_t num_sectors,
			frigg::CallbackPtr<void()> callback) {
//	printf("readSectors(%lu, %lu)\n", sector, num_sectors);

	UserRequest *user_request = new UserRequest;
	user_request->buffer = buffer;
	user_request->callback = callback;
	user_request->countdown = num_sectors;

	for(size_t i = 0; i < num_sectors; i++) {
		// send a single request
		assert(!bufferStack.empty());
		size_t buffer_index = bufferStack.back();
		bufferStack.pop_back();

		auto virt_request = (VirtRequest *)(requestSpace + buffer_index * 0x400);
		virt_request->type = VIRTIO_BLK_T_IN;
		virt_request->reserved = 0;
		virt_request->sector = sector + i;

		PseudoDescriptor pseudo[3];
		pseudo[0].address = 0xA000 + buffer_index * 0x400;
		pseudo[0].length = 16;
		pseudo[0].flags = 0;

		pseudo[1].address = 0xA010 + buffer_index * 0x400;
		pseudo[1].length = 512;
		pseudo[1].flags = VIRTQ_DESC_F_WRITE;
		
		pseudo[2].address = 0xA210 + buffer_index * 0x400;
		pseudo[2].length = 1;
		pseudo[2].flags = VIRTQ_DESC_F_WRITE;
		
		RequestSlot slot;
		slot.slotType = RequestSlot::kSlotRead;
		slot.userRequest = user_request;
		slot.bufferIndex = buffer_index;
		slot.part = i;
		
		queue0.postRequest(pseudo, 3, slot);
	}

	queue0.notifyDevice();
}

Device device;

void initLegacyDevice() {
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

	// perform device specific setup
	queue0.setupQueue(0x8000);

	// finally set the DRIVER_OK bit to finish the configuration
	frigg::writeIo<uint8_t>(basePort + PCI_L_DEVICE_STATUS,
			frigg::readIo<uint8_t>(basePort + PCI_L_DEVICE_STATUS) | DRIVER_OK);
	
	libfs::runDevice(eventHub, &device);
}

helx::Irq irq;

void onInterrupt(void *unused, HelError error) {
	HEL_CHECK(error);

	frigg::readIo<uint16_t>(basePort + PCI_L_ISR_STATUS);
	queue0.process();

	irq.wait(eventHub, CALLBACK_STATIC(nullptr, &onInterrupt));
}

// --------------------------------------------------------
// InitClosure
// --------------------------------------------------------

struct InitClosure {
	void operator() ();

private:
	void connected();
	void enumeratedDevice(std::vector<bragi_mbus::ObjectId> objects);
	void queriredDevice(HelHandle handle);
};

void InitClosure::operator() () {
	mbusConnection.connect(CALLBACK_MEMBER(this, &InitClosure::connected));
}

void InitClosure::connected() {
	mbusConnection.enumerate("pci-vendor:0x1af4",
			CALLBACK_MEMBER(this, &InitClosure::enumeratedDevice));
}

void InitClosure::enumeratedDevice(std::vector<bragi_mbus::ObjectId> objects) {
	assert(objects.size() == 1);
	mbusConnection.queryIf(objects[0],
			CALLBACK_MEMBER(this, &InitClosure::queriredDevice));
}

void InitClosure::queriredDevice(HelHandle handle) {
	helx::Pipe device_pipe(handle);

	// acquire the device's resources
	HelError acquire_error;
	uint8_t acquire_buffer[128];
	size_t acquire_length;
	device_pipe.recvStringRespSync(acquire_buffer, 128, eventHub, 1, 0,
			acquire_error, acquire_length);
	HEL_CHECK(acquire_error);

	managarm::hw::PciDevice acquire_response;
	acquire_response.ParseFromArray(acquire_buffer, acquire_length);

	HelError bar_error;
	HelHandle bar_handle;
	device_pipe.recvDescriptorRespSync(eventHub, 1, 1, bar_error, bar_handle);
	HEL_CHECK(bar_error);

	HEL_CHECK(helEnableIo(bar_handle));
	
/*	// map the BAR into this address space
	assert(acquire_response.bars(1).io_type() == managarm::hw::IoType::MEMORY);
	void *ptr;
	HEL_CHECK(helMapMemory(bar_handle, kHelNullHandle, nullptr,
			acquire_response.bars(1).length(), kHelMapReadWrite, &ptr));
	config = (VirtCommonCfg *)ptr;*/
	
	irq = helx::Irq::access(11);
	irq.wait(eventHub, CALLBACK_STATIC(nullptr, &onInterrupt));
	
	initLegacyDevice();
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting virtio driver\n");

	// allocate memory to hold the requests
	// FIXME: use a different memory region
	HelHandle memory;
	void *pointer;
	HEL_CHECK(helAccessPhysical(0xA000, 0x40000, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
			0x40000, kHelMapReadWrite, &pointer));
	requestSpace = (char *)pointer;

	for(size_t i = 0; i < 256; i++)
		bufferStack.push_back(i);

	auto closure = new InitClosure();
	(*closure)();

	while(true)
		eventHub.defaultProcessEvents();	
}

