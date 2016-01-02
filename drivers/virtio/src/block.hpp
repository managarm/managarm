
#include <queue>
#include <helx.hpp>
#include <libfs.hpp>

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

// --------------------------------------------------------
// UserRequest
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

// --------------------------------------------------------
// UserRequest
// --------------------------------------------------------

struct Device : public GenericDevice, public libfs::BlockDevice {
	Device();

	void readSectors(uint64_t sector, void *buffer, size_t num_sectors,
			frigg::CallbackPtr<void()> callback) override;

	void doInitialize() override;
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

} } // namespace virtio::block

