
#include <queue>
#include <blockfs.hpp>

#include "virtio.hpp"

namespace virtio {
namespace block {

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

struct Device;

// --------------------------------------------------------
// UserRequest
// --------------------------------------------------------

struct UserRequest : Request {
	UserRequest(uint64_t sector, void *buffer, size_t num_sectors);

	uint64_t sector;
	void *buffer;
	size_t numSectors;

	async::promise<void> promise;
};

// --------------------------------------------------------
// Device
// --------------------------------------------------------

struct Device : blockfs::BlockDevice {
	Device(std::unique_ptr<Transport> transport);

	void runDevice();

	async::result<void> readSectors(uint64_t sector,
			void *buffer, size_t num_sectors) override;

private:
	// Submits requests from _pendingQueue to the device.
	cofiber::no_future _processRequests();
	
	cofiber::no_future _processIrqs();

	std::unique_ptr<Transport> _transport;

	// The single virtq of this device.
	Queue _requestQueue;

	// Stores UserRequest objects that have not been submitted yet.
	std::queue<UserRequest *> _pendingQueue;
	async::doorbell _pendingDoorbell;

	// these two buffer store virtio-block request header and status bytes
	// they are indexed by the index of the request's first descriptor
	VirtRequest *virtRequestBuffer;
	uint8_t *statusBuffer;
};

} } // namespace virtio::block

