
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vector>

#include <arch/dma_structs.hpp>
#include <arch/io_space.hpp>
#include <arch/mem_space.hpp>
#include <arch/register.hpp>
#include <arch/variable.hpp>
#include <async/doorbell.hpp>
#include <helix/ipc.hpp>
#include <protocols/hw/client.hpp>

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

enum {
	// Bits of the spec::Descriptor::flags field.
	VIRTQ_DESC_F_NEXT = 1, // descriptor is part of a chain
	VIRTQ_DESC_F_WRITE = 2, // buffer is written by device

	// Bits of the spec::UsedRing::flags field.
	VIRTQ_USED_F_NO_NOTIFY = 1 // no need to notify the device
};

namespace spec {
	struct Descriptor {
		arch::scalar_variable<uint64_t> address;
		arch::scalar_variable<uint32_t> length;
		arch::scalar_variable<uint16_t> flags;
		arch::scalar_variable<uint16_t> next;
	};

	struct AvailableRing {
		arch::scalar_variable<uint16_t> flags;
		arch::scalar_variable<uint16_t> headIndex;
		
		struct Element {
			arch::scalar_variable<uint16_t> tableIndex;
		} elements[];
	};

	struct AvailableExtra {
		arch::scalar_variable<uint16_t> eventIndex;
	};

	struct UsedRing {
		arch::scalar_variable<uint16_t> flags;
		arch::scalar_variable<uint16_t> headIndex;
	
		struct Element {
			arch::scalar_variable<uint32_t> tableIndex;
			arch::scalar_variable<uint32_t> written;
		} elements[];
	};

	struct UsedExtra {
		arch::scalar_variable<uint16_t> eventIndex;
	};
};

struct Queue;

// --------------------------------------------------------
// Transport
// --------------------------------------------------------

struct Transport {
	virtual void setupDevice() = 0;

	virtual helix::BorrowedDescriptor getIrq() = 0;
	
	virtual void readIsr() = 0;

	virtual size_t queryQueue(unsigned int queue_index) = 0;
	
	virtual void setupQueue(unsigned int queue_index, uintptr_t physical) = 0;

	virtual void notifyQueue(unsigned int queue_index) = 0;
};

async::result<std::unique_ptr<Transport>> discover(protocols::hw::Device hw_device);

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

	void setupBuffer(HostToDeviceType, arch::dma_buffer_view view);
	void setupBuffer(DeviceToHostType, arch::dma_buffer_view view);

	void setupLink(Handle other);

private:
	Queue *_queue;
	size_t _tableIndex;
};

struct Request {
	void (*complete)(Request *);
};

// Represents a single virtq.
struct Queue {
	friend struct Handle;

	Queue(Transport *transport, size_t queue_index);

	// Initializes the virtq. Call this during driver initialization.
	void setupQueue();

	// Returns the number of descriptors in this virtq.
	size_t numDescriptors();

	// Allocates a single descriptor.
	// The descriptor is automatically freed when the device returns it.
	async::result<Handle> obtainDescriptor();

	// Posts a descriptor to the virtq's available ring.
	void postDescriptor(Handle descriptor, Request *request,
			void (*complete)(Request *));

	// Notifies the device that new descriptors have been posted.
	void notifyDevice();

	// Processes interrupts for this virtq.
	// Calls retrieveDescriptor() to complete individual requests.
	void processInterrupt();

private:
	Transport *_transport;

	// Index of this queue as part of its owning device.
	size_t _queueIndex;
	
	// Number of descriptors in this queue.
	size_t _queueSize;

	// Pointers to different data structures of this virtq.
	spec::Descriptor *_table;
	spec::AvailableRing *_availableRing;
	spec::AvailableExtra *_availableExtra;
	spec::UsedRing *_usedRing;
	spec::UsedExtra *_usedExtra;

	// Keeps track of unused descriptor indices.
	std::vector<uint16_t> _descriptorStack;

	async::doorbell _descriptorDoorbell;

	std::vector<Request *> _activeRequests;

	// Keeps track of which entries in the used ring have already been processed.
	uint16_t _progressHead;
};

} // namespace virtio

