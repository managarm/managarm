
#include <memory>

#include <blockfs.hpp>
#include <core/virtio/core.hpp>
#include <async/oneshot-event.hpp>

namespace block {
namespace virtio {

// --------------------------------------------------------
// VirtIO data structures and constants
// --------------------------------------------------------

// Natural align to guarantee that we do not cross page boundaries.
struct alignas(16) VirtRequest {
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
};
static_assert(sizeof(VirtRequest) == 16, "Bad sizeof(VirtRequest)");

enum {
	VIRTIO_BLK_T_IN = 0,
	VIRTIO_BLK_T_OUT = 1,
	VIRTIO_BLK_T_FLUSH = 4,
};

enum {
	VIRTIO_BLK_F_FLUSH = 9
};

enum {
	VIRTIO_BLK_S_OK = 0,
	VIRTIO_BLK_S_IOERR = 1,
	VIRTIO_BLK_S_UNSUPP = 2
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
	UserRequest(bool write, uint64_t sector, arch::dma_buffer_view view,
			arch::dma_pool *pool);

	bool write;
	uint64_t sector;
	arch::dma_buffer_view view;

	// Request header and status byte of this request.
	arch::dma_object<VirtRequest> header;
	arch::dma_object<uint8_t> status;

	async::oneshot_primitive event;
};

// --------------------------------------------------------
// Device
// --------------------------------------------------------

struct Device : blockfs::BlockDevice {
	Device(std::unique_ptr<virtio_core::Transport> transport, int64_t parent_id);

	async::result<void> runDevice();

	async::result<void> readSectors(uint64_t sector, arch::dma_buffer_view view) override;
	async::result<void> writeSectors(uint64_t sector, arch::dma_buffer_view view) override;

	async::result<void> flush() override;

	async::result<size_t> getSize() override;

private:
	// Sets up the descriptor chain of the request and posts it to the device.
	// Returns after submission without waiting for the request's completion.
	async::result<void> _issueRequest(UserRequest *request);

	std::unique_ptr<virtio_core::Transport> _transport;

	// The single virtq of this device.
	virtio_core::Queue *_requestQueue;

	// The size of the disk
	size_t _size;

	// Whether the device supports VIRTIO_BLK_T_FLUSH.
	bool _hasFlush = false;
};

} } // namespace block::virtio

