#include <linux/virtio_gpu.h>

#include "src/commands.hpp"
#include "src/spec.hpp"
#include "src/virtio.hpp"

struct AwaitableRequest : virtio_core::Request {
	static void complete(virtio_core::Request *base) {
		auto self = static_cast<AwaitableRequest *>(base);
		self->_handle.resume();
	}

	AwaitableRequest(virtio_core::Queue *queue, virtio_core::Handle descriptor)
	: _queue{queue}, _descriptor{descriptor} { }

	bool await_ready() {
		return false;
	}

	void await_suspend(std::coroutine_handle<> handle) {
		_handle = handle;

		_queue->postDescriptor(_descriptor, this, &AwaitableRequest::complete);
		_queue->notify();
	}

	void await_resume() {
	}

private:
	virtio_core::Queue *_queue;
	virtio_core::Handle _descriptor;
	std::coroutine_handle<> _handle;
};

async::result<void> Cmd::transferToHost2d(uint32_t width, uint32_t height, uint32_t resourceId, GfxDevice *device) {
	arch::dma_object<spec::XferToHost2d> xfer{
	    &device->dmaPool(),
	    spec::XferToHost2d{
	        .header{.type = spec::cmd::xferToHost2d},
	        .rect{
	            .x = 0,
	            .y = 0,
	            .width = width,
	            .height = height,
	        },
	        .resourceId = resourceId,
	    }
	};

	arch::dma_object<spec::Header> xfer_result{
	    &device->dmaPool(),
	    spec::Header{}
	};
	virtio_core::Chain xfer_chain;
	co_await virtio_core::scatterGather(virtio_core::hostToDevice,
			xfer_chain, device->_controlQ,
			xfer.view_buffer());
	co_await virtio_core::scatterGather(virtio_core::deviceToHost,
			xfer_chain, device->_controlQ,
			xfer_result.view_buffer());
	co_await AwaitableRequest{device->_controlQ, xfer_chain.front()};
	assert(xfer_result->type == spec::resp::noData);
}

async::result<void> Cmd::setScanout(uint32_t width, uint32_t height, uint32_t scanoutId, uint32_t resourceId, GfxDevice *device) {
	arch::dma_object<spec::SetScanout> scanout{
	    &device->dmaPool(),
	    spec::SetScanout{
	        .header{.type = spec::cmd::setScanout},
	        .rect{
	            .x = 0,
	            .y = 0,
	            .width = width,
	            .height = height,
	        },
	        .scanoutId = scanoutId,
	        .resourceId = resourceId,
	    }
	};

	arch::dma_object<spec::Header> scanout_result{
	    &device->dmaPool(),
	    spec::Header{}
	};
	virtio_core::Chain scanout_chain;
	co_await virtio_core::scatterGather(virtio_core::hostToDevice,
			scanout_chain, device->_controlQ,
			scanout.view_buffer());
	co_await virtio_core::scatterGather(virtio_core::deviceToHost,
			scanout_chain, device->_controlQ,
			scanout_result.view_buffer());
	co_await AwaitableRequest{device->_controlQ, scanout_chain.front()};

	assert(scanout_result->type == spec::resp::noData);
}

async::result<void> Cmd::resourceFlush(uint32_t width, uint32_t height, uint32_t resourceId, GfxDevice *device) {
	arch::dma_object<spec::ResourceFlush> flush{
	    &device->dmaPool(),
	    spec::ResourceFlush{
	        .header{.type = spec::cmd::resourceFlush},
	        .rect{
	            .x = 0,
	            .y = 0,
	            .width = width,
	            .height = height,
	        },
	        .resourceId = resourceId,
	    }
	};

	arch::dma_object<spec::Header> flush_result{
	    &device->dmaPool(),
	    spec::Header{}
	};
	virtio_core::Chain flush_chain;
	co_await virtio_core::scatterGather(virtio_core::hostToDevice, flush_chain, device->_controlQ,
		flush.view_buffer());
	co_await virtio_core::scatterGather(virtio_core::deviceToHost, flush_chain, device->_controlQ,
		flush_result.view_buffer());
	co_await AwaitableRequest{device->_controlQ, flush_chain.front()};

	assert(flush_result->type == spec::resp::noData);
}

async::result<arch::dma_object<spec::DisplayInfo>> Cmd::getDisplayInfo(GfxDevice *device) {
	arch::dma_object<spec::Header> header{
	    &device->dmaPool(),
	    spec::Header{
	        .type = spec::cmd::getDisplayInfo,
	        .flags = 0,
	        .fenceId = 0,
	        .contextId = 0,
	        .padding = 0,
	    }
	};

	arch::dma_object<spec::DisplayInfo> info{
	    &device->dmaPool(),
	    spec::DisplayInfo{}
	};

	virtio_core::Chain chain;
	co_await virtio_core::scatterGather(virtio_core::hostToDevice, chain, device->_controlQ,
			header.view_buffer());
	co_await virtio_core::scatterGather(virtio_core::deviceToHost, chain, device->_controlQ,
			info.view_buffer());

	co_await AwaitableRequest{device->_controlQ, chain.front()};

	co_return std::move(info);
}

async::result<void> Cmd::create2d(uint32_t width, uint32_t height, uint32_t resourceId, GfxDevice *device) {
	arch::dma_object<spec::Create2d> buffer{
	    &device->dmaPool(),
	    spec::Create2d{
	        .header{.type = spec::cmd::create2d},
	        .resourceId = resourceId,
	        .format = spec::format::bgrx,
	        .width = width,
	        .height = height,
	    }
	};

	arch::dma_object<spec::Header> result{
	    &device->dmaPool(),
	    spec::Header{}
	};
	virtio_core::Chain chain;
	co_await virtio_core::scatterGather(virtio_core::hostToDevice, chain, device->_controlQ,
			buffer.view_buffer());
	co_await virtio_core::scatterGather(virtio_core::deviceToHost, chain, device->_controlQ,
			result.view_buffer());
	co_await AwaitableRequest{device->_controlQ, chain.front()};

	assert(result->type == spec::resp::noData);
}

async::result<void> Cmd::attachBacking(uint32_t resourceId, arch::dma_buffer_view view, GfxDevice *device) {
	assert(view.size());
	assert((reinterpret_cast<uintptr_t>(view.data()) & 0xFFF) == 0);
	auto &space = device->dmaSpace();
	size_t pages = (view.size() + 0xFFF) >> 12;
	size_t memEntries = space.iommuActive() ? 1 : pages;

	arch::dma_array<spec::MemEntry> entries{&device->dmaPool(), memEntries};
	if (space.iommuActive()) {
		entries[0].address = co_await device->dmaSpace().iova_of(view);
		entries[0].length = pages << 12;
	} else {
		for(size_t page = 0; page < pages; page++) {
			uintptr_t physical = co_await device->dmaSpace().iova_of(view.subview(page << 12, 0x1000));
			entries[page].address = physical;
			entries[page].length = 4096;
		}
	}

	arch::dma_object<spec::AttachBacking> attachment{
	    &device->dmaPool(),
	    spec::AttachBacking{
	        .header{.type = spec::cmd::attachBacking},
	        .resourceId = resourceId,
	        .numEntries = static_cast<uint32_t>(entries.size()),
	    }
	};

	arch::dma_object<spec::Header> attach_result{
	    &device->dmaPool(),
	    spec::Header{}
	};
	virtio_core::Chain attach_chain;
	co_await virtio_core::scatterGather(virtio_core::hostToDevice, attach_chain, device->_controlQ,
			attachment.view_buffer());
	co_await virtio_core::scatterGather(virtio_core::hostToDevice, attach_chain, device->_controlQ,
			entries.view_buffer());
	co_await virtio_core::scatterGather(virtio_core::deviceToHost, attach_chain, device->_controlQ,
			attach_result.view_buffer());
	co_await AwaitableRequest{device->_controlQ, attach_chain.front()};

	assert(attach_result->type == spec::resp::noData);
}
