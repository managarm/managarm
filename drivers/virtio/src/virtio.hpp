
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

inline constexpr arch::scalar_register<uint32_t> PCI_L_DEVICE_FEATURES(0);
inline constexpr arch::scalar_register<uint32_t> PCI_L_DRIVER_FEATURES(4);
inline constexpr arch::scalar_register<uint32_t> PCI_L_QUEUE_ADDRESS(8);
inline constexpr arch::scalar_register<uint16_t> PCI_L_QUEUE_SIZE(12);
inline constexpr arch::scalar_register<uint16_t> PCI_L_QUEUE_SELECT(14);
inline constexpr arch::scalar_register<uint16_t> PCI_L_QUEUE_NOTIFY(16);
inline constexpr arch::scalar_register<uint8_t> PCI_L_DEVICE_STATUS(18);
inline constexpr arch::scalar_register<uint8_t> PCI_L_ISR_STATUS(19);

inline constexpr arch::scalar_register<uint32_t> PCI_DEVICE_FEATURE_SELECT(0);
inline constexpr arch::scalar_register<uint32_t> PCI_DEVICE_FEATURE_WINDOW(4);
inline constexpr arch::scalar_register<uint32_t> PCI_DRIVER_FEATURE_SELECT(8);
inline constexpr arch::scalar_register<uint32_t> PCI_DRIVER_FEATURE_WINDOW(12);
inline constexpr arch::scalar_register<uint8_t> PCI_DEVICE_STATUS(20);
inline constexpr arch::scalar_register<uint16_t> PCI_QUEUE_SELECT(22);
inline constexpr arch::scalar_register<uint16_t> PCI_QUEUE_SIZE(24);
inline constexpr arch::scalar_register<uint16_t> PCI_QUEUE_ENABLE(28);
inline constexpr arch::scalar_register<uint16_t> PCI_QUEUE_NOTIFY(30);
inline constexpr arch::scalar_register<uint32_t> PCI_QUEUE_TABLE[] = {
		arch::scalar_register<uint32_t>{32},
		arch::scalar_register<uint32_t>{36}};
inline constexpr arch::scalar_register<uint32_t> PCI_QUEUE_AVAILABLE[] = {
		arch::scalar_register<uint32_t>{40},
		arch::scalar_register<uint32_t>{44}};
inline constexpr arch::scalar_register<uint32_t> PCI_QUEUE_USED[] = {
		arch::scalar_register<uint32_t>{48},
		arch::scalar_register<uint32_t>{52}};

inline constexpr arch::scalar_register<uint8_t> PCI_ISR(0);

enum {
	PCI_L_DEVICE_SPECIFIC = 20
};

// bits of the device status register
enum {
	ACKNOWLEDGE = 1,
	DRIVER = 2,
	FEATURES_OK = 8,
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
	static_assert(sizeof(Descriptor) == 16);

	struct AvailableRing {
		arch::scalar_variable<uint16_t> flags;
		arch::scalar_variable<uint16_t> headIndex;
		
		struct Element {
			arch::scalar_variable<uint16_t> tableIndex;
		} elements[];
	};
	static_assert(sizeof(AvailableRing) == 4);

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
	static_assert(sizeof(UsedRing) == 4);

	struct UsedExtra {
		arch::scalar_variable<uint16_t> eventIndex;
	};
};

struct Queue;

// --------------------------------------------------------
// Transport
// --------------------------------------------------------

struct QueueInfo {
	size_t queueSize;
	ptrdiff_t notifyOffset;
};

/* This class represents a virtio device.
 * 
 * Usual initialization works as follows:
 * - Call discover() to obtain a transport.
 * - Call Transport::finalizeFeatures().
 * - Call Queue::setupQueue() for each virtq.
 * - Call Transport::runDevice().
 */
struct Transport {
	virtual void finalizeFeatures() = 0;
	virtual void runDevice() = 0;

	virtual helix::BorrowedDescriptor getIrq() = 0;
	
	virtual void readIsr() = 0;

	// The following functions are used by the Queue class.
	// Usually you should not use them directly.
	virtual QueueInfo queryQueueInfo(unsigned int queue_index) = 0;
	virtual void setupQueue(unsigned int queue_index, uintptr_t table_physical,
			uintptr_t available_physical, uintptr_t used_physical) = 0;
	virtual void notifyDevice(unsigned int queue_index, ptrdiff_t notify_offset) = 0;
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
	// We need to pass this to Transport::notifyDevice().
	ptrdiff_t _notifyOffset;

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

