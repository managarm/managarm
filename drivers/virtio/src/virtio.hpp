
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vector>

#include <arch/io_space.hpp>
#include <arch/register.hpp>
#include <helix/ipc.hpp>

namespace virtio {

// --------------------------------------------------------
// VirtIO data structures and constants
// --------------------------------------------------------

static arch::scalar_register<uint32_t> PCI_L_DEVICE_FEATURES(0);
static arch::scalar_register<uint32_t> PCI_L_DRIVER_FEATURES(4);
static arch::scalar_register<uint32_t> PCI_L_QUEUE_ADDRESS(8);
static arch::scalar_register<uint16_t> PCI_L_QUEUE_SIZE(12);
static arch::scalar_register<uint16_t> PCI_L_QUEUE_SELECT(14);
static arch::scalar_register<uint16_t> PCI_L_QUEUE_NOTIFY(16);
static arch::scalar_register<uint8_t> PCI_L_DEVICE_STATUS(18);
static arch::scalar_register<uint8_t> PCI_L_ISR_STATUS(19);

enum {
	PCI_L_DEVICE_SPECIFIC = 20
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

enum {
	// bits of the VirtDescriptor::flags field
	VIRTQ_DESC_F_NEXT = 1, // descriptor is part of a chain
	VIRTQ_DESC_F_WRITE = 2, // buffer is written by device

	// bits of the VirtUsedHeader::flags field
	VIRTQ_USED_F_NO_NOTIFY = 1 // no need to notify the device
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

// --------------------------------------------------------
// GenericDevice
// --------------------------------------------------------


struct GenericDevice {
	friend class Queue;

	GenericDevice();

	void setupDevice(uint16_t base_port, helix::UniqueDescriptor the_interrupt);

	uint8_t readIsr();

	uint8_t readConfig8(size_t offset);

	// performs device specific initialization
	// this is called after features are negotiated
	virtual void doInitialize() = 0;

	virtual void retrieveDescriptor(size_t queue_index,
			size_t desc_index, size_t bytes_written) = 0;

	virtual void afterRetrieve() = 0;

private:
	arch::io_space space;

protected:
	helix::UniqueDescriptor interrupt;
};

// --------------------------------------------------------
// Queue
// --------------------------------------------------------

struct Queue {
	Queue(GenericDevice &device, size_t queue_index);

	VirtDescriptor *accessDescriptor(size_t index);

	// initializes the virtqueue. called during driver initialization
	void setupQueue();

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

} // namespace virtio

