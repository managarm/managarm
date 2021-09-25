
#include <queue>

#include <blockfs.hpp>
#include <core/virtio/core.hpp>
#include <async/oneshot-event.hpp>

namespace block {
namespace virtio {

// --------------------------------------------------------
// VirtIO data structures and constants
// --------------------------------------------------------

struct VirtRequest {
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
};
static_assert(sizeof(VirtRequest) == 16, "Bad sizeof(VirtRequest)");

enum {
	VIRTIO_BLK_T_IN = 0,
	VIRTIO_BLK_T_OUT = 1
};

namespace spec::regs {
	inline constexpr arch::scalar_register<uint32_t> capacity[] = {
			arch::scalar_register<uint32_t>{0},
			arch::scalar_register<uint32_t>{4}};
}

struct Device;

// --------------------------------------------------------
// UserRequest
// --------------------------------------------------------

struct UserRequest : virtio_core::Request {
	UserRequest(bool write, uint64_t sector, void *buffer, size_t num_sectors);

	bool write;
	uint64_t sector;
	void *buffer;
	size_t numSectors;

	async::oneshot_event event;
};

// --------------------------------------------------------
// Device
// --------------------------------------------------------

struct Device : blockfs::BlockDevice {
	Device(std::unique_ptr<virtio_core::Transport> transport);

	void runDevice();

	async::result<void> readSectors(uint64_t sector,
			void *buffer, size_t num_sectors) override;

	async::result<void> writeSectors(uint64_t sector,
			const void *buffer, size_t num_sectors) override;

private:
	// Submits requests from _pendingQueue to the device.
	async::detached _processRequests();
	
	std::unique_ptr<virtio_core::Transport> _transport;

	// The single virtq of this device.
	virtio_core::Queue *_requestQueue;

	// Stores UserRequest objects that have not been submitted yet.
	std::queue<UserRequest *> _pendingQueue;
	async::recurring_event _pendingDoorbell;

	// these two buffer store virtio-block request header and status bytes
	// they are indexed by the index of the request's first descriptor
	VirtRequest *virtRequestBuffer;
	uint8_t *statusBuffer;
};

} } // namespace block::virtio

