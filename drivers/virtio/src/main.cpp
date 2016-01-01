
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <vector>
#include <queue>
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
// Queue
// --------------------------------------------------------

struct GenericDevice {
	virtual void retrieveDescriptor(size_t queue_index, size_t desc_index) = 0;

	virtual void afterRetrieve() = 0;
};

struct Queue {
	Queue(GenericDevice &device, size_t queue_index);

	VirtDescriptor *accessDescriptor(size_t index);

	// initializes the virtqueue. called during driver initialization
	void setupQueue(uintptr_t physical);

	// returns the number of descriptors in this virtqueue
	size_t getSize();

	// returns the number of unused descriptors
	size_t numLockable();

	// allocates a single descriptor
	// the descriptor is freed when the device returns via the virtqueue's used ring
	size_t lockDescriptor();

	// posts a descriptor to the virtqueue's available ring
	void postDescriptor(size_t desc_index);

	// notifies the device that new descriptors have been posted to the available ring
	void notifyDevice();

	// processes interrupts for this virtqueue
	// calls retrieveDescriptor() to complete individual requests
	void processInterrupt();

private:
	static constexpr size_t kQueueAlign = 0x1000;

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

	GenericDevice &device;

	// index of this queue relativate to its owning device
	size_t queueIndex;
	
	// number of descriptors in this queue
	size_t queueSize;
	
	// pointers to different data structures of this queue
	char *descriptorPtr, *availPtr, *usedPtr;

	// keeps track of unused descriptor indices
	std::vector<uint16_t> descriptorStack;

	// keeps track of which entries in the used ring have already been processed
	uint16_t progressHead;
};

Queue::Queue(GenericDevice &device, size_t queue_index)
: device(device), queueIndex(queue_index), queueSize(0),
		descriptorPtr(nullptr), availPtr(nullptr), usedPtr(nullptr),
		progressHead(0) { }

VirtDescriptor *Queue::accessDescriptor(size_t index) {
	return reinterpret_cast<VirtDescriptor *>(descriptorPtr + index * sizeof(VirtDescriptor));
}

void Queue::setupQueue(uintptr_t physical) {
	assert(!queueSize);

	// select the queue and determine it's size
	frigg::writeIo<uint16_t>(basePort + PCI_L_QUEUE_SELECT, 0);
	queueSize = frigg::readIo<uint16_t>(basePort + PCI_L_QUEUE_SIZE);
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
	frigg::writeIo<uint32_t>(basePort + PCI_L_QUEUE_ADDRESS, physical / 0x1000);
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
	frigg::writeIo<uint16_t>(basePort + PCI_L_QUEUE_NOTIFY, queueIndex);
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
		device.retrieveDescriptor(queueIndex, desc_index);

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
};
static_assert(sizeof(VirtRequest) == 16, "Bad sizeof(VirtRequest)");

// --------------------------------------------------------

struct UserRequest {
	UserRequest(uint64_t sector, void *buffer, size_t num_sectors,
			frigg::CallbackPtr<void()> callback);

	uint64_t sector;
	void *buffer;
	size_t numSectors;
	frigg::CallbackPtr<void()> callback;

	size_t numSubmitted;
	size_t sectorsRead;
};

UserRequest::UserRequest(uint64_t sector, void *buffer, size_t num_sectors,
		frigg::CallbackPtr<void()> callback)
: sector(sector), buffer(buffer), numSectors(num_sectors), callback(callback),
		numSubmitted(0), sectorsRead(0) { }

struct Device : public GenericDevice, public libfs::BlockDevice {
	Device();

	void initialize();

	void readSectors(uint64_t sector, void *buffer, size_t num_sectors,
			frigg::CallbackPtr<void()> callback) override;

	void retrieveDescriptor(size_t queue_index, size_t desc_index) override;

	void afterRetrieve() override;

	void onInterrupt(HelError error);

private:
	// returns true iff the request can be submitted to the device
	bool requestIsReady(UserRequest *user_request);
	
	// submits a single request to the device
	void submitRequest(UserRequest *user_request);

	// the single virtqueue of this virtio-block device
	Queue requestQueue;

	// IRQ of this device
	helx::Irq irq;

	// these two buffer store virtio-block request header and status bytes
	// they are indexed by the index of the request's first descriptor
	VirtRequest *virtRequestBuffer;
	uint8_t *statusBuffer;

	// memorizes UserRequest objects that have been submitted to the queue
	// indexed by the index of the request's first descriptor
	std::vector<UserRequest *> userRequestPtrs;

	// stores UserRequest objects that have not been submitted yet
	std::queue<UserRequest *> pendingRequests;
	
	// stores UserRequest objects that were retrieved and completed
	std::vector<UserRequest *> completeStack;
};

Device::Device()
: libfs::BlockDevice(512), requestQueue(*this, 0) { }

void Device::initialize() {
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
	requestQueue.setupQueue(0x8000);
	userRequestPtrs.resize(requestQueue.getSize());
	virtRequestBuffer = (VirtRequest *)malloc(requestQueue.getSize() * sizeof(VirtRequest));
	statusBuffer = (uint8_t *)malloc(requestQueue.getSize());

	// natural alignment makes sure that request headers do not cross page boundaries
	assert((uintptr_t)virtRequestBuffer % sizeof(VirtRequest) == 0);
	
	// setup an interrupt for the device
	irq = helx::Irq::access(11);
	irq.wait(eventHub, CALLBACK_MEMBER(this, &Device::onInterrupt));

	// finally set the DRIVER_OK bit to finish the configuration
	frigg::writeIo<uint8_t>(basePort + PCI_L_DEVICE_STATUS,
			frigg::readIo<uint8_t>(basePort + PCI_L_DEVICE_STATUS) | DRIVER_OK);
	
	libfs::runDevice(eventHub, this);
}

void Device::readSectors(uint64_t sector, void *buffer, size_t num_sectors,
			frigg::CallbackPtr<void()> callback) {
	// natural alignment makes sure a sector does not cross a page boundary
	assert(((uintptr_t)buffer % 512) == 0);

//	printf("readSectors(%lu, %lu)\n", sector, num_sectors);

	UserRequest *user_request = new UserRequest(sector, buffer, num_sectors, callback);
	assert(pendingRequests.empty());
	assert(requestIsReady(user_request));
	submitRequest(user_request);
}

void Device::retrieveDescriptor(size_t queue_index, size_t desc_index) {
	assert(queue_index == 0);

	UserRequest *user_request = userRequestPtrs[desc_index];
	assert(user_request);
	userRequestPtrs[desc_index] = nullptr;

	// check the status byte
	assert(user_request->numSubmitted > 0);
	assert(statusBuffer[desc_index] == 0);

	user_request->sectorsRead += user_request->numSubmitted;
	user_request->numSubmitted = 0;

	// re-submit the request if it is not complete yet
	if(user_request->sectorsRead < user_request->numSectors) {
		pendingRequests.push(user_request);
	}else{
		completeStack.push_back(user_request);
	}
}

void Device::afterRetrieve() {
	while(!pendingRequests.empty()) {
		UserRequest *user_request = pendingRequests.front();
		if(!requestIsReady(user_request))
			break;
		submitRequest(user_request);
		pendingRequests.pop();
	}

	while(!completeStack.empty()) {
		UserRequest *user_request = completeStack.back();
		user_request->callback();
		delete user_request;
		completeStack.pop_back();
	}
}

void Device::onInterrupt(HelError error) {
	HEL_CHECK(error);

	frigg::readIo<uint16_t>(basePort + PCI_L_ISR_STATUS);
	requestQueue.processInterrupt();

	irq.wait(eventHub, CALLBACK_MEMBER(this, &Device::onInterrupt));
}

bool Device::requestIsReady(UserRequest *user_request) {
	return requestQueue.numLockable() > 2;
}

void Device::submitRequest(UserRequest *user_request) {
	assert(user_request->numSubmitted == 0);
	assert(user_request->sectorsRead < user_request->numSectors);

	// setup the actual request header
	size_t header_index = requestQueue.lockDescriptor();

	VirtRequest *header = &virtRequestBuffer[header_index];
	header->type = VIRTIO_BLK_T_IN;
	header->reserved = 0;
	header->sector = user_request->sector + user_request->sectorsRead;

	// setup a descriptor for the request header
	uintptr_t header_physical;
	HEL_CHECK(helPointerPhysical(header, &header_physical));
	
	VirtDescriptor *header_desc = requestQueue.accessDescriptor(header_index);
	header_desc->address = header_physical;
	header_desc->length = sizeof(VirtRequest);
	header_desc->flags = 0;

	size_t num_lockable = requestQueue.numLockable();
	assert(num_lockable > 2);
	size_t max_data_chain = num_lockable - 2;

	// setup descriptors for the transfered data
	VirtDescriptor *chain_desc = header_desc;
	for(size_t i = 0; i < max_data_chain; i++) {
		size_t offset = user_request->sectorsRead + user_request->numSubmitted;
		if(offset == user_request->numSectors)
			break;
		assert(offset < user_request->numSectors);
		
		uintptr_t data_physical;
		HEL_CHECK(helPointerPhysical((char *)user_request->buffer + offset * 512,
				&data_physical));

		size_t data_index = requestQueue.lockDescriptor();
		VirtDescriptor *data_desc = requestQueue.accessDescriptor(data_index);
		data_desc->address = data_physical;
		data_desc->length = 512;
		data_desc->flags = VIRTQ_DESC_F_WRITE;

		chain_desc->flags |= VIRTQ_DESC_F_NEXT;
		chain_desc->next = data_index;

		user_request->numSubmitted++;
		chain_desc = data_desc;
	}
//	printf("Submitting %lu data descriptors\n", user_request->numSubmitted);

	// setup a descriptor for the status byte	
	uintptr_t status_physical;
	HEL_CHECK(helPointerPhysical(&statusBuffer[header_index], &status_physical));
	
	size_t status_index = requestQueue.lockDescriptor();
	VirtDescriptor *status_desc = requestQueue.accessDescriptor(status_index);
	status_desc->address = status_physical;
	status_desc->length = 1;
	status_desc->flags = VIRTQ_DESC_F_WRITE;

	chain_desc->flags |= VIRTQ_DESC_F_NEXT;
	chain_desc->next = status_index;

	// submit the request to the device
	assert(!userRequestPtrs[header_index]);
	userRequestPtrs[header_index] = user_request;
	requestQueue.postDescriptor(header_index);
	requestQueue.notifyDevice();
}

Device device;

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
	
	device.initialize();
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting virtio driver\n");

	auto closure = new InitClosure();
	(*closure)();

	while(true)
		eventHub.defaultProcessEvents();	
}

