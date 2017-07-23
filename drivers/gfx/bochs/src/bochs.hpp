
#include <queue>

#include <arch/mem_space.hpp>
#include <async/doorbell.hpp>
#include <async/mutex.hpp>
#include <async/result.hpp>

#include "spec.hpp"

struct GfxDevice : std::enable_shared_from_this<GfxDevice> {
	GfxDevice(void* frame_buffer);
	
	cofiber::no_future initialize();
	
private:
	arch::io_space _operational;
	void* _frameBuffer;
};

