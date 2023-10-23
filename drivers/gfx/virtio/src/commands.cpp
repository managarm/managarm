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
	spec::XferToHost2d xfer;
	memset(&xfer, 0, sizeof(spec::XferToHost2d));
	xfer.header.type = spec::cmd::xferToHost2d;
	xfer.rect.x = 0;
	xfer.rect.y = 0;
	xfer.rect.width = width;
	xfer.rect.height = height;
	xfer.resourceId = resourceId;

	spec::Header xfer_result;
	virtio_core::Chain xfer_chain;
	co_await virtio_core::scatterGather(virtio_core::hostToDevice,
			xfer_chain, device->_controlQ,
			arch::dma_buffer_view{nullptr, &xfer, sizeof(spec::XferToHost2d)});
	co_await virtio_core::scatterGather(virtio_core::deviceToHost,
			xfer_chain, device->_controlQ,
			arch::dma_buffer_view{nullptr, &xfer_result, sizeof(spec::Header)});
	co_await AwaitableRequest{device->_controlQ, xfer_chain.front()};
	assert(xfer_result.type == spec::resp::noData);
}

async::result<void> Cmd::setScanout(uint32_t width, uint32_t height, uint32_t scanoutId, uint32_t resourceId, GfxDevice *device) {
	spec::SetScanout scanout;
	memset(&scanout, 0, sizeof(spec::SetScanout));
	scanout.header.type = spec::cmd::setScanout;
	scanout.rect.x = 0;
	scanout.rect.y = 0;
	scanout.rect.width = width;
	scanout.rect.height = height;
	scanout.scanoutId = scanoutId;
	scanout.resourceId = resourceId;

	spec::Header scanout_result;
	virtio_core::Chain scanout_chain;
	co_await virtio_core::scatterGather(virtio_core::hostToDevice,
			scanout_chain, device->_controlQ,
			arch::dma_buffer_view{nullptr, &scanout, sizeof(spec::SetScanout)});
	co_await virtio_core::scatterGather(virtio_core::deviceToHost,
			scanout_chain, device->_controlQ,
			arch::dma_buffer_view{nullptr, &scanout_result, sizeof(spec::Header)});
	co_await AwaitableRequest{device->_controlQ, scanout_chain.front()};

	assert(scanout_result.type == spec::resp::noData);
}

async::result<void> Cmd::resourceFlush(uint32_t width, uint32_t height, uint32_t resourceId, GfxDevice *device) {
	spec::ResourceFlush flush;
	memset(&flush, 0, sizeof(spec::ResourceFlush));
	flush.header.type = spec::cmd::resourceFlush;
	flush.rect.x = 0;
	flush.rect.y = 0;
	flush.rect.width = width;
	flush.rect.height = height;
	flush.resourceId = resourceId;

	spec::Header flush_result;
	virtio_core::Chain flush_chain;
	co_await virtio_core::scatterGather(virtio_core::hostToDevice, flush_chain, device->_controlQ,
		arch::dma_buffer_view{nullptr, &flush, sizeof(spec::ResourceFlush)});
	co_await virtio_core::scatterGather(virtio_core::deviceToHost, flush_chain, device->_controlQ,
		arch::dma_buffer_view{nullptr, &flush_result, sizeof(spec::Header)});
	co_await AwaitableRequest{device->_controlQ, flush_chain.front()};

	assert(flush_result.type == spec::resp::noData);
}

async::result<spec::DisplayInfo> Cmd::getDisplayInfo(GfxDevice *device) {
	spec::Header header;
	header.type = spec::cmd::getDisplayInfo;
	header.flags = 0;
	header.fenceId = 0;
	header.contextId = 0;
	header.padding = 0;

	spec::DisplayInfo info;

	virtio_core::Chain chain;
	co_await virtio_core::scatterGather(virtio_core::hostToDevice, chain, device->_controlQ,
			arch::dma_buffer_view{nullptr, &header, sizeof(spec::Header)});
	co_await virtio_core::scatterGather(virtio_core::deviceToHost, chain, device->_controlQ,
			arch::dma_buffer_view{nullptr, &info, sizeof(spec::DisplayInfo)});

	co_await AwaitableRequest{device->_controlQ, chain.front()};

	co_return info;
}

async::result<void> Cmd::create2d(uint32_t width, uint32_t height, uint32_t resourceId, GfxDevice *device) {
	spec::Create2d buffer;
	memset(&buffer, 0, sizeof(spec::Create2d));
	buffer.header.type = spec::cmd::create2d;
	buffer.resourceId = resourceId;
	buffer.format = spec::format::bgrx;
	buffer.width = width;
	buffer.height = height;

	spec::Header result;
	virtio_core::Chain chain;
	co_await virtio_core::scatterGather(virtio_core::hostToDevice, chain, device->_controlQ,
			arch::dma_buffer_view{nullptr, &buffer, sizeof(spec::Create2d)});
	co_await virtio_core::scatterGather(virtio_core::deviceToHost, chain, device->_controlQ,
			arch::dma_buffer_view{nullptr, &result, sizeof(spec::Header)});
	co_await AwaitableRequest{device->_controlQ, chain.front()};

	assert(result.type == spec::resp::noData);
}

async::result<void> Cmd::attachBacking(uint32_t resourceId, void *ptr, size_t size, GfxDevice *device) {
	assert(ptr);
	
	std::vector<spec::MemEntry> entries;
	for(size_t page = 0; page < size; page += 4096) {
		spec::MemEntry entry;
		memset(&entry, 0, sizeof(spec::MemEntry));
		uintptr_t physical;

		HEL_CHECK(helPointerPhysical((reinterpret_cast<char *>(ptr) + page), &physical));
		entry.address = physical;
		entry.length = 4096;
		entries.push_back(entry);
	}

	spec::AttachBacking attachment;
	memset(&attachment, 0, sizeof(spec::AttachBacking));
	attachment.header.type = spec::cmd::attachBacking;
	attachment.resourceId = resourceId;
	attachment.numEntries = entries.size();

	spec::Header attach_result;
	virtio_core::Chain attach_chain;
	co_await virtio_core::scatterGather(virtio_core::hostToDevice, attach_chain, device->_controlQ,
			arch::dma_buffer_view{nullptr, &attachment, sizeof(spec::AttachBacking)});
	co_await virtio_core::scatterGather(virtio_core::hostToDevice, attach_chain, device->_controlQ,
			arch::dma_buffer_view{nullptr, entries.data(), entries.size() * sizeof(spec::MemEntry)});
	co_await virtio_core::scatterGather(virtio_core::deviceToHost, attach_chain, device->_controlQ,
			arch::dma_buffer_view{nullptr, &attach_result, sizeof(spec::Header)});
	co_await AwaitableRequest{device->_controlQ, attach_chain.front()};

	assert(attach_result.type == spec::resp::noData);
}
