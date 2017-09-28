
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vector>

#include <arch/io_space.hpp>
#include <arch/register.hpp>
#include <async/doorbell.hpp>
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

struct VirtAvailableHeader {
	uint16_t flags;
	uint16_t headIndex;
};
struct VirtAvailableRing {
	uint16_t descIndex;
};
struct VirtAvailableFooter {
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

struct Queue;

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

struct HostToDeviceType { };
struct DeviceToHostType { };

inline constexpr HostToDeviceType hostToDevice;
inline constexpr DeviceToHostType deviceToHost;

// Handle to a virtq descriptor.
struct Handle {
	Handle(Queue *queue, size_t table_index);
	
	size_t tableIndex() {
		return _tableIndex;
	}

	void setupBuffer(HostToDeviceType, const void *buffer, size_t size);
	void setupBuffer(DeviceToHostType, void *buffer, size_t size);
	void setupLink(Handle other);

private:
	Queue *_queue;
	size_t _tableIndex;
};

// Represents a single virtq.
struct Queue {
	Queue(GenericDevice *device, size_t queue_index);

	VirtDescriptor *accessDescriptor(size_t index);

	// Initializes the virtq. Called this during driver initialization.
	void setupQueue();

	// Returns the number of descriptors in this virtq.
	size_t numDescriptors();

	// Returns the number of unused descriptors.
	size_t numUnusedDescriptors();

	// Allocates a single descriptor.
	// The descriptor is automatically freed when the device returns it.
	async::result<Handle> obtainDescriptor();

	// Posts a descriptor to the virtq's available ring.
	void postDescriptor(Handle descriptor);

	// Notifies the device that new descriptors have been posted.
	void notifyDevice();

	// Processes interrupts for this virtq.
	// Calls retrieveDescriptor() to complete individual requests.
	void processInterrupt();

private:
	static constexpr size_t kQueueAlign = 0x1000;

	auto accessAvailableHeader() {
		return reinterpret_cast<VirtAvailableHeader *>(_availablePtr);
	}
	auto accessAvailableRing(size_t index) {
		return reinterpret_cast<VirtAvailableRing *>(_availablePtr + sizeof(VirtAvailableHeader)
				+ index * sizeof(VirtAvailableRing));
	}
	auto accessAvailableFooter() {
		return reinterpret_cast<VirtAvailableFooter *>(_availablePtr + sizeof(VirtAvailableHeader)
				+ _queueSize * sizeof(VirtAvailableRing));
	}

	auto accessUsedHeader() {
		return reinterpret_cast<VirtUsedHeader *>(_usedPtr);
	}
	auto accessUsedRing(size_t index) {
		return reinterpret_cast<VirtUsedRing *>(_usedPtr + sizeof(VirtUsedHeader)
				+ index * sizeof(VirtUsedRing));
	}
	auto accessUsedFooter() {
		return reinterpret_cast<VirtUsedFooter *>(_usedPtr + sizeof(VirtUsedHeader)
				+ _queueSize * sizeof(VirtUsedRing));
	}

	GenericDevice *_device;

	// Index of this queue as part of its owning device.
	size_t _queueIndex;
	
	// Number of descriptors in this queue.
	size_t _queueSize;
	
	// Pointers to different data structures of this virtq.
	char *_descriptorPtr, *_availablePtr, *_usedPtr;

	// Keeps track of unused descriptor indices.
	std::vector<uint16_t> _descriptorStack;

	async::doorbell _descriptorDoorbell;

	// Keeps track of which entries in the used ring have already been processed.
	uint16_t _progressHead;
};

} // namespace virtio

