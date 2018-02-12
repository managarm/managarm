
#include <assert.h>
#include <stdio.h>
#include <deque>
#include <experimental/optional>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>

#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <arch/io_space.hpp>
#include <async/result.hpp>
#include <boost/intrusive/list.hpp>
#include <cofiber.hpp>
#include <frigg/atomic.hpp>
#include <frigg/arch_x86/machine.hpp>
#include <frigg/memory.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <core/drm/core.hpp>

#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>

#include "virtio.hpp"
#include <fs.pb.h>

constexpr auto fileOperations = protocols::fs::FileOperations{}
	.withRead(&drm_core::File::read)
	.withAccessMemory(&drm_core::File::accessMemory)
	.withIoctl(&drm_core::File::ioctl)
	.withPoll(&drm_core::File::poll);

COFIBER_ROUTINE(cofiber::no_future, serveDevice(std::shared_ptr<drm_core::Device> device,
		helix::UniqueLane p), ([device = std::move(device), lane = std::move(p)] {
	std::cout << "unix device: Connection" << std::endl;

	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();
		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor push_node;
			
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			auto file = std::make_shared<drm_core::File>(device);
			protocols::fs::servePassthrough(std::move(local_lane), file,
					&fileOperations);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&push_node, remote_lane));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else{
			throw std::runtime_error("Invalid request in serveDevice()");
		}
	}
}))

// ----------------------------------------------------------------
// GfxDevice.
// ----------------------------------------------------------------

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

	void await_suspend(cofiber::coroutine_handle<> handle) {
		_handle = handle;

		_queue->postDescriptor(_descriptor, this, &AwaitableRequest::complete);
		_queue->notify();
	}

	void await_resume() {
	}

private:
	virtio_core::Queue *_queue;
	virtio_core::Handle _descriptor;
	cofiber::coroutine_handle<> _handle;
};

GfxDevice::GfxDevice(std::unique_ptr<virtio_core::Transport> transport)
: _transport(std::move(transport)) { }

COFIBER_ROUTINE(cofiber::no_future, GfxDevice::initialize(), ([=] { 
	_transport->finalizeFeatures();
	_transport->claimQueues(2);

	_controlQ = _transport->setupQueue(0);
	_cursorQ = _transport->setupQueue(1);

	_transport->runDevice();

	auto num_scanouts = static_cast<uint32_t>(_transport->space().load(spec::cfg::numScanouts));
	for(size_t i = 0; i < num_scanouts; i++) {
		auto plane = std::make_shared<Plane>(this, i);
		auto crtc = std::make_shared<Crtc>(this, i, plane);
		auto encoder = std::make_shared<Encoder>(this);
		
		plane->setupWeakPtr(plane);
		crtc->setupWeakPtr(crtc);
		encoder->setupWeakPtr(encoder);
	
		encoder->setupPossibleCrtcs({crtc.get()});
		encoder->setupPossibleClones({encoder.get()});
		encoder->setCurrentCrtc(crtc.get());

		registerObject(plane.get());
		registerObject(crtc.get());
		registerObject(encoder.get());

		setupCrtc(crtc.get());
		setupEncoder(encoder.get());

		_theCrtcs[i] = crtc;
		_theEncoders[i] = encoder;
	}
	
	spec::Header header;
	header.type = spec::cmd::getDisplayInfo;
	header.flags = 0;
	header.fenceId = 0;
	header.contextId = 0;
	header.padding = 0;
	
	spec::DisplayInfo info;
	virtio_core::Request request;
	
	virtio_core::Chain chain;
	COFIBER_AWAIT virtio_core::scatterGather(virtio_core::hostToDevice, chain, _controlQ,
			arch::dma_buffer_view{nullptr, &header, sizeof(spec::Header)});
	COFIBER_AWAIT virtio_core::scatterGather(virtio_core::deviceToHost, chain, _controlQ,
			arch::dma_buffer_view{nullptr, &info, sizeof(spec::DisplayInfo)});

	COFIBER_AWAIT AwaitableRequest{_controlQ, chain.front()};
	for(size_t i = 0; i < 16; i++) {
		if(info.modes[i].enabled) {
			auto connector = std::make_shared<Connector>(this);
			connector->setupWeakPtr(connector);
			
			connector->setCurrentEncoder(_theEncoders[i].get());
			connector->setCurrentStatus(1);
			
			registerObject(connector.get());
			attachConnector(connector.get());
			
			std::vector<drm_mode_modeinfo> supported_modes;
			drm_core::addDmtModes(supported_modes, info.modes[i].rect.width,
					info.modes[i].rect.height);
			connector->setModeList(supported_modes);

			_activeConnectors[i] = connector;
		}
	}
}))
	
std::unique_ptr<drm_core::Configuration> GfxDevice::createConfiguration() {
	return std::make_unique<Configuration>(this);
}

std::shared_ptr<drm_core::FrameBuffer> GfxDevice::createFrameBuffer(std::shared_ptr<drm_core::BufferObject> base_bo,
		uint32_t width, uint32_t height, uint32_t format, uint32_t pitch) {
	auto bo = std::static_pointer_cast<GfxDevice::BufferObject>(base_bo);
		
	assert(pitch % 4 == 0);
	
	assert(pitch / 4 >= width);
	assert(bo->getSize() >= pitch * height);

	auto fb = std::make_shared<FrameBuffer>(this, bo);
	fb->setupWeakPtr(fb);
	registerObject(fb.get());
	return fb;
}

std::tuple<int, int, int> GfxDevice::driverVersion() {
	return {0, 0, 1};
}

std::tuple<std::string, std::string, std::string> GfxDevice::driverInfo() {
	return {"virtio_gpu", "virtio GPU", "0"};
}

std::pair<std::shared_ptr<drm_core::BufferObject>, uint32_t>
GfxDevice::createDumb(uint32_t width, uint32_t height, uint32_t bpp) {
	HelHandle handle;
	auto size = ((width * height * bpp / 8) + (4096 - 1)) & ~(4096 - 1);
	HEL_CHECK(helAllocateMemory(size, 0, &handle));

	auto bo = std::make_shared<BufferObject>(this, _hwAllocator.allocate(), size, helix::UniqueDescriptor(handle),
			width, height);
	uint32_t pitch = width * bpp / 8;
	
	auto mapping = installMapping(bo.get());
	bo->setupMapping(mapping);
	
	bo->_initHw();
	return std::make_pair(bo, pitch);
}

// ----------------------------------------------------------------
// GfxDevice::Configuration.
// ----------------------------------------------------------------

bool GfxDevice::Configuration::capture(std::vector<drm_core::Assignment> assignment) {
	auto captureScanout = [&] (int index) {
		if(_state[index])
			return;

		_state[index].emplace();
		// TODO: Capture FB, width and height.
		_state[index]->mode = _device->_theCrtcs[index]->currentMode();
	};

	for(auto &assign : assignment) {
		if(assign.property == _device->srcWProperty()) {
			//TODO: check this outside of capure
			assert(assign.property->validate(assign));
			
			auto plane = static_cast<Plane *>(assign.object.get());
			captureScanout(plane->scanoutId());
			_state[plane->scanoutId()]->width = assign.intValue;
		}else if(assign.property == _device->srcHProperty()) {
			//TODO: check this outside of capure
			assert(assign.property->validate(assign));

			auto plane = static_cast<Plane *>(assign.object.get());
			captureScanout(plane->scanoutId());
			_state[plane->scanoutId()]->height = assign.intValue;
		}else if(assign.property == _device->fbIdProperty()) {
			//TODO: check this outside of capure
			assert(assign.property->validate(assign));

			auto plane = static_cast<Plane *>(assign.object.get());
			auto fb = assign.objectValue->asFrameBuffer();
			captureScanout(plane->scanoutId());
			_state[plane->scanoutId()]->fb = static_cast<GfxDevice::FrameBuffer *>(fb);
		}else if(assign.property == _device->modeIdProperty()) {
			//TODO: check this outside of capture.
			assert(assign.property->validate(assign));
		
			auto crtc = static_cast<Crtc *>(assign.object.get());
			captureScanout(crtc->scanoutId());
			_state[crtc->scanoutId()]->mode = assign.blobValue;
		}else{
			return false;
		}
	}

	for(size_t i = 0; i < _state.size(); i++) {
		if(!_state[i])
			continue;

		if(_state[i]->mode) {
			// TODO: Consider current width/height if FB did not change.
			drm_mode_modeinfo mode_info;
			memcpy(&mode_info, _state[i]->mode->data(), sizeof(drm_mode_modeinfo));
			_state[i]->height = mode_info.vdisplay;
			_state[i]->width = mode_info.hdisplay;
			
			if(_state[i]->width <= 0 || _state[i]->height <= 0 
					|| _state[i]->width > 1024 || _state[i]->height > 768)
				return false;
			if(!_state[i]->fb)
				return false;
		}
	}
	return true;
}

void GfxDevice::Configuration::dispose() {

}

void GfxDevice::Configuration::commit() {
	for(size_t i = 0; i < _state.size(); i++) {
		if(!_state[i])
			continue;
		_device->_theCrtcs[i]->setCurrentMode(_state[i]->mode);
	}

	_dispatch();
}

COFIBER_ROUTINE(cofiber::no_future, GfxDevice::Configuration::_dispatch(), ([=] {
	for(size_t i = 0; i < _state.size(); i++) {
		if(!_state[i])
			continue;

		if(!_state[i]->mode) {
			spec::SetScanout scanout;
			memset(&scanout, 0, sizeof(spec::SetScanout));
			scanout.header.type = spec::cmd::setScanout;

			spec::Header scanout_result;
			virtio_core::Request scanout_request;	
			virtio_core::Chain scanout_chain;
			COFIBER_AWAIT virtio_core::scatterGather(virtio_core::hostToDevice,
					scanout_chain, _device->_controlQ,
					arch::dma_buffer_view{nullptr, &scanout, sizeof(spec::SetScanout)});
			COFIBER_AWAIT virtio_core::scatterGather(virtio_core::deviceToHost,
					scanout_chain, _device->_controlQ,
					arch::dma_buffer_view{nullptr, &scanout_result, sizeof(spec::Header)});
			COFIBER_AWAIT AwaitableRequest{_device->_controlQ, scanout_chain.front()};
			
			continue;
		}

		COFIBER_AWAIT _state[i]->fb->getBufferObject()->wait();

		spec::XferToHost2d xfer;
		memset(&xfer, 0, sizeof(spec::XferToHost2d));
		xfer.header.type = spec::cmd::xferToHost2d;
		xfer.rect.x = 0;
		xfer.rect.y = 0;
		xfer.rect.width = _state[i]->width;
		xfer.rect.height = _state[i]->height;
		xfer.resourceId = _state[i]->fb->getBufferObject()->hardwareId();

		spec::Header xfer_result;
		virtio_core::Request xfer_request;	
		virtio_core::Chain xfer_chain;
		COFIBER_AWAIT virtio_core::scatterGather(virtio_core::hostToDevice,
				xfer_chain, _device->_controlQ,
				arch::dma_buffer_view{nullptr, &xfer, sizeof(spec::XferToHost2d)});
		COFIBER_AWAIT virtio_core::scatterGather(virtio_core::deviceToHost,
				xfer_chain, _device->_controlQ,
				arch::dma_buffer_view{nullptr, &xfer_result, sizeof(spec::Header)});
		COFIBER_AWAIT AwaitableRequest{_device->_controlQ, xfer_chain.front()};


		spec::SetScanout scanout;
		memset(&scanout, 0, sizeof(spec::SetScanout));
		scanout.header.type = spec::cmd::setScanout;
		scanout.rect.x = 0;
		scanout.rect.y = 0;
		scanout.rect.width = _state[i]->width;
		scanout.rect.height = _state[i]->height;
		scanout.scanoutId = i;
		scanout.resourceId = _state[i]->fb->getBufferObject()->hardwareId();

		spec::Header scanout_result;
		virtio_core::Request scanout_request;	
		virtio_core::Chain scanout_chain;
		COFIBER_AWAIT virtio_core::scatterGather(virtio_core::hostToDevice,
				scanout_chain, _device->_controlQ,
				arch::dma_buffer_view{nullptr, &scanout, sizeof(spec::SetScanout)});
		COFIBER_AWAIT virtio_core::scatterGather(virtio_core::deviceToHost,
				scanout_chain, _device->_controlQ,
				arch::dma_buffer_view{nullptr, &scanout_result, sizeof(spec::Header)});
		COFIBER_AWAIT AwaitableRequest{_device->_controlQ, scanout_chain.front()};


		spec::ResourceFlush flush;
		memset(&flush, 0, sizeof(spec::ResourceFlush));
		flush.header.type = spec::cmd::resourceFlush;
		flush.rect.x = 0;
		flush.rect.y = 0;
		flush.rect.width = _state[i]->width;
		flush.rect.height = _state[i]->height;
		flush.resourceId = _state[i]->fb->getBufferObject()->hardwareId();

		spec::Header flush_result;
		virtio_core::Request flush_request;	
		virtio_core::Chain flush_chain;
		COFIBER_AWAIT virtio_core::scatterGather(virtio_core::hostToDevice,
				flush_chain, _device->_controlQ,
				arch::dma_buffer_view{nullptr, &flush, sizeof(spec::ResourceFlush)});
		COFIBER_AWAIT virtio_core::scatterGather(virtio_core::deviceToHost,
				flush_chain, _device->_controlQ,
				arch::dma_buffer_view{nullptr, &flush_result, sizeof(spec::Header)});
		COFIBER_AWAIT AwaitableRequest{_device->_controlQ, flush_chain.front()};
	}

	complete();
}))

// ----------------------------------------------------------------
// GfxDevice::Connector.
// ----------------------------------------------------------------

GfxDevice::Connector::Connector(GfxDevice *device)
	: drm_core::Connector { device->allocator.allocate() } {
//	_encoders.push_back(device->_theEncoder.get());
}

// ----------------------------------------------------------------
// GfxDevice::Encoder.
// ----------------------------------------------------------------

GfxDevice::Encoder::Encoder(GfxDevice *device)
	:drm_core::Encoder { device->allocator.allocate() } {
}

// ----------------------------------------------------------------
// GfxDevice::Crtc.
// ----------------------------------------------------------------

GfxDevice::Crtc::Crtc(GfxDevice *device, int id, std::shared_ptr<Plane> plane)
	:drm_core::Crtc { device->allocator.allocate() } {
	_device = device;
	_scanoutId = id;
	_primaryPlane = plane;
}

drm_core::Plane *GfxDevice::Crtc::primaryPlane() {
	return _primaryPlane.get();
}

int GfxDevice::Crtc::scanoutId() {
	return _scanoutId;
}

// ----------------------------------------------------------------
// GfxDevice::FrameBuffer.
// ----------------------------------------------------------------

GfxDevice::FrameBuffer::FrameBuffer(GfxDevice *device,
		std::shared_ptr<GfxDevice::BufferObject> bo)
: drm_core::FrameBuffer { device->allocator.allocate() } {
	_bo = bo;
	_device = device;
}

GfxDevice::BufferObject *GfxDevice::FrameBuffer::getBufferObject() {
	return _bo.get();
}

void GfxDevice::FrameBuffer::notifyDirty() {
	_xferAndFlush();
}

COFIBER_ROUTINE(cofiber::no_future, GfxDevice::FrameBuffer::_xferAndFlush(), ([=] {
	spec::XferToHost2d xfer;
	memset(&xfer, 0, sizeof(spec::XferToHost2d));
	xfer.header.type = spec::cmd::xferToHost2d;
	xfer.rect.x = 0;
	xfer.rect.y = 0;
	xfer.rect.width = _bo->getWidth();
	xfer.rect.height = _bo->getHeight();
	xfer.resourceId = _bo->hardwareId();

	spec::Header xfer_result;
	virtio_core::Request xfer_request;	
	virtio_core::Chain xfer_chain;
	COFIBER_AWAIT virtio_core::scatterGather(virtio_core::hostToDevice, xfer_chain, _device->_controlQ,
		arch::dma_buffer_view{nullptr, &xfer, sizeof(spec::XferToHost2d)});
	COFIBER_AWAIT virtio_core::scatterGather(virtio_core::deviceToHost, xfer_chain, _device->_controlQ,
		arch::dma_buffer_view{nullptr, &xfer_result, sizeof(spec::Header)});
	COFIBER_AWAIT AwaitableRequest{_device->_controlQ, xfer_chain.front()};
		
	spec::ResourceFlush flush;
	memset(&flush, 0, sizeof(spec::ResourceFlush));
	flush.header.type = spec::cmd::resourceFlush;
	flush.rect.x = 0;
	flush.rect.y = 0;
	flush.rect.width = _bo->getWidth();
	flush.rect.height = _bo->getHeight();
	flush.resourceId = _bo->hardwareId();

	spec::Header flush_result;
	virtio_core::Request flush_request;	
	virtio_core::Chain flush_chain;
	COFIBER_AWAIT virtio_core::scatterGather(virtio_core::hostToDevice, flush_chain, _device->_controlQ,
		arch::dma_buffer_view{nullptr, &flush, sizeof(spec::ResourceFlush)});
	COFIBER_AWAIT virtio_core::scatterGather(virtio_core::deviceToHost, flush_chain, _device->_controlQ,
		arch::dma_buffer_view{nullptr, &flush_result, sizeof(spec::Header)});
	COFIBER_AWAIT AwaitableRequest{_device->_controlQ, flush_chain.front()};
}));

// ----------------------------------------------------------------
// GfxDevice: Plane.
// ----------------------------------------------------------------

GfxDevice::Plane::Plane(GfxDevice *device, int id)
	:drm_core::Plane { device->allocator.allocate() } {
	_scanoutId = id;
}

int GfxDevice::Plane::scanoutId() {
	return _scanoutId;
}

// ----------------------------------------------------------------
// GfxDevice: BufferObject.
// ----------------------------------------------------------------

std::shared_ptr<drm_core::BufferObject> GfxDevice::BufferObject::sharedBufferObject() {
	return this->shared_from_this();
}

size_t GfxDevice::BufferObject::getSize() {
	return _size;
}

uint32_t GfxDevice::BufferObject::getWidth() {
	return _width;
}

uint32_t GfxDevice::BufferObject::getHeight() {
	return _height;
}

uint32_t GfxDevice::BufferObject::hardwareId() {
	return _hardwareId;
}

async::result<void> GfxDevice::BufferObject::wait() {
	return _jump.async_wait();
}

std::pair<helix::BorrowedDescriptor, uint64_t> GfxDevice::BufferObject::getMemory() {
	return std::make_pair(helix::BorrowedDescriptor(_memory), 0);
}

COFIBER_ROUTINE(cofiber::no_future, GfxDevice::BufferObject::_initHw(), ([=] { 
	void *ptr;
	HEL_CHECK(helMapMemory(_memory.getHandle(), kHelNullHandle,
		nullptr, 0, getSize(), kHelMapProtRead, &ptr));

	spec::Create2d buffer;
	memset(&buffer, 0, sizeof(spec::Create2d));
	buffer.header.type = spec::cmd::create2d;
	buffer.resourceId = _hardwareId;
	buffer.format = spec::format::bgrx;
	buffer.width = getWidth();
	buffer.height = getHeight();
	spec::Header result;
	
	virtio_core::Request request;	
	virtio_core::Chain chain;
	COFIBER_AWAIT virtio_core::scatterGather(virtio_core::hostToDevice, chain, _device->_controlQ,
			arch::dma_buffer_view{nullptr, &buffer, sizeof(spec::Create2d)});
	COFIBER_AWAIT virtio_core::scatterGather(virtio_core::deviceToHost, chain, _device->_controlQ,
			arch::dma_buffer_view{nullptr, &result, sizeof(spec::Header)});
	COFIBER_AWAIT AwaitableRequest{_device->_controlQ, chain.front()};

	
	std::vector<spec::MemEntry> entries;
	for(size_t page = 0; page < getSize(); page += 4096) {
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
	attachment.resourceId = _hardwareId;
	attachment.numEntries = entries.size();
	spec::Header attach_result;
	
	virtio_core::Request attach_request;
	virtio_core::Chain attach_chain;
	COFIBER_AWAIT virtio_core::scatterGather(virtio_core::hostToDevice, attach_chain, _device->_controlQ,
			arch::dma_buffer_view{nullptr, &attachment, sizeof(spec::AttachBacking)});
	COFIBER_AWAIT virtio_core::scatterGather(virtio_core::hostToDevice, attach_chain, _device->_controlQ,
			arch::dma_buffer_view{nullptr, entries.data(), entries.size() * sizeof(spec::MemEntry)});
	COFIBER_AWAIT virtio_core::scatterGather(virtio_core::deviceToHost, attach_chain, _device->_controlQ,
			arch::dma_buffer_view{nullptr, &attach_result, sizeof(spec::Header)});
	COFIBER_AWAIT AwaitableRequest{_device->_controlQ, attach_chain.front()};

	_jump.trigger();
}));

// ----------------------------------------------------------------
// Freestanding PCI discovery functions.
// ----------------------------------------------------------------

COFIBER_ROUTINE(cofiber::no_future, bindController(mbus::Entity entity), ([=] {
	protocols::hw::Device hw_device(COFIBER_AWAIT entity.bind());
	auto transport = COFIBER_AWAIT virtio_core::discover(std::move(hw_device),
			virtio_core::DiscoverMode::modernOnly);

	auto gfx_device = std::make_shared<GfxDevice>(std::move(transport));
	gfx_device->initialize();

	// Create an mbus object for the device.
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();
	
	mbus::Properties descriptor{
		{"unix.subsystem", mbus::StringItem{"drm"}},
		{"unix.devname", mbus::StringItem{"dri/card0"}}
	};

	auto handler = mbus::ObjectHandler{}
	.withBind([=] () -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		serveDevice(gfx_device, std::move(local_lane));

		async::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.async_get();
	});

	COFIBER_AWAIT root.createObject("gfx_virtio", descriptor, std::move(handler));
}))

COFIBER_ROUTINE(cofiber::no_future, observeControllers(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-vendor", "1af4"),
		mbus::EqualsFilter("pci-device", "1050")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) {
		std::cout << "gfx/virtio: Detected device" << std::endl;
		bindController(std::move(entity));
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
}))

int main() {
	std::cout << "gfx/virtio: Starting driver" << std::endl;

	observeControllers();

	while(true)
		helix::Dispatcher::global().dispatch();
	
	return 0;
}

