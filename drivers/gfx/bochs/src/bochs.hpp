
#include <queue>

#include <arch/mem_space.hpp>
#include <async/doorbell.hpp>
#include <async/mutex.hpp>
#include <async/result.hpp>

#include "spec.hpp"

// ----------------------------------------------------------------
// Stuff that belongs in a DRM library.
// ----------------------------------------------------------------

struct DrmDevice {
	static async::result<int64_t> seek(std::shared_ptr<void> object, int64_t offset);
	static async::result<size_t> read(std::shared_ptr<void> object, void *buffer, size_t length);
	static async::result<void> write(std::shared_ptr<void> object,
			const void *buffer, size_t length);
	static async::result<helix::BorrowedDescriptor> accessMemory(std::shared_ptr<void> object);
	static async::result<void> ioctl(std::shared_ptr<void> object, managarm::fs::CntRequest req,
			helix::UniqueLane conversation);
};

// ----------------------------------------------------------------

struct GfxDevice : DrmDevice, std::enable_shared_from_this<GfxDevice> {
	GfxDevice(helix::UniqueDescriptor video_ram, void* frame_buffer);
	
	cofiber::no_future initialize();

public:
	// FIX ME: this is a hack	
	helix::UniqueDescriptor _videoRam;
private:
	arch::io_space _operational;
	void* _frameBuffer;
};

