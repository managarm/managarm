
#include <assert.h>
#include <stdio.h>
#include <deque>
#include <optional>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>

#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/svrctl/server.hpp>
#include <core/drm/core.hpp>

#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>

#include "src/commands.hpp"
#include "virtio.hpp"
#include <fs.bragi.hpp>

// Maps mbus IDs to device objects
std::unordered_map<int64_t, std::shared_ptr<GfxDevice>> baseDeviceMap;

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

GfxDevice::GfxDevice(std::unique_ptr<virtio_core::Transport> transport)
: _transport{std::move(transport)}, _claimedDevice{false} { }

async::detached GfxDevice::initialize() {
	_transport->finalizeFeatures();
	_transport->claimQueues(2);

	_controlQ = _transport->setupQueue(0);
	_cursorQ = _transport->setupQueue(1);

	_transport->runDevice();

	std::vector<drm_core::Assignment> assignments;

	auto num_scanouts = static_cast<uint32_t>(_transport->space().load(spec::cfg::numScanouts));
	for(size_t i = 0; i < num_scanouts; i++) {
		auto plane = std::make_shared<Plane>(this, i, Plane::PlaneType::PRIMARY);
		auto crtc = std::make_shared<Crtc>(this, i, plane);
		auto encoder = std::make_shared<Encoder>(this);

		plane->setupWeakPtr(plane);
		plane->setupState(plane);
		crtc->setupWeakPtr(crtc);
		crtc->setupState(crtc);
		encoder->setupWeakPtr(encoder);

		plane->setupPossibleCrtcs({crtc.get()});

		encoder->setupPossibleCrtcs({crtc.get()});
		encoder->setupPossibleClones({encoder.get()});
		encoder->setCurrentCrtc(crtc.get());

		registerObject(plane.get());
		registerObject(crtc.get());
		registerObject(encoder.get());

		assignments.push_back(drm_core::Assignment::withInt(crtc, activeProperty(), 0));

		assignments.push_back(drm_core::Assignment::withInt(plane, planeTypeProperty(), 1));
		assignments.push_back(drm_core::Assignment::withModeObj(plane, crtcIdProperty(), crtc));
		assignments.push_back(drm_core::Assignment::withInt(plane, srcHProperty(), 0));
		assignments.push_back(drm_core::Assignment::withInt(plane, srcWProperty(), 0));
		assignments.push_back(drm_core::Assignment::withInt(plane, crtcHProperty(), 0));
		assignments.push_back(drm_core::Assignment::withInt(plane, crtcWProperty(), 0));
		assignments.push_back(drm_core::Assignment::withInt(plane, srcXProperty(), 0));
		assignments.push_back(drm_core::Assignment::withInt(plane, srcYProperty(), 0));
		assignments.push_back(drm_core::Assignment::withInt(plane, crtcXProperty(), 0));
		assignments.push_back(drm_core::Assignment::withInt(plane, crtcYProperty(), 0));
		assignments.push_back(drm_core::Assignment::withModeObj(plane, fbIdProperty(), nullptr));

		setupCrtc(crtc.get());
		setupEncoder(encoder.get());

		_theCrtcs[i] = crtc;
		_theEncoders[i] = encoder;
	}

	auto info = co_await Cmd::getDisplayInfo(this);

	for(size_t i = 0; i < 16; i++) {
		if(info.modes[i].enabled) {
			auto connector = std::make_shared<Connector>(this);
			connector->setupWeakPtr(connector);
			connector->setupState(connector);

			connector->setupPossibleEncoders({_theEncoders[i].get()});
			connector->setCurrentEncoder(_theEncoders[i].get());
			connector->setCurrentStatus(1);
			connector->setConnectorType(DRM_MODE_CONNECTOR_VIRTUAL);

			registerObject(connector.get());
			attachConnector(connector.get());

			assignments.push_back(drm_core::Assignment::withInt(connector, dpmsProperty(), 3));
			assignments.push_back(drm_core::Assignment::withModeObj(connector, crtcIdProperty(), nullptr));

			std::vector<drm_mode_modeinfo> supported_modes;
			drm_core::addDmtModes(supported_modes, info.modes[i].rect.width,
					info.modes[i].rect.height);
			std::sort(supported_modes.begin(), supported_modes.end(),
					[] (const drm_mode_modeinfo &u, const drm_mode_modeinfo &v) {
				return u.hdisplay * u.vdisplay > v.hdisplay * v.vdisplay;
			});
			connector->setModeList(supported_modes);

			_activeConnectors[i] = connector;
		}
	}

	auto config = createConfiguration();
	auto state = atomicState();
	assert(config->capture(assignments, state));
	config->commit(state);
	co_await config->waitForCompletion();
}

std::unique_ptr<drm_core::Configuration> GfxDevice::createConfiguration() {
	return std::make_unique<Configuration>(this);
}

std::shared_ptr<drm_core::FrameBuffer> GfxDevice::createFrameBuffer(std::shared_ptr<drm_core::BufferObject> base_bo,
		uint32_t width, uint32_t height, uint32_t, uint32_t pitch) {
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
	HEL_CHECK(helAllocateMemory(size, 0, nullptr, &handle));

	auto bo = std::make_shared<BufferObject>(this, _hwAllocator.allocate(), size,
			helix::UniqueDescriptor(handle), width, height);
	uint32_t pitch = width * bpp / 8;

	auto mapping = installMapping(bo.get());
	bo->setupMapping(mapping);

	bo->_initHw();
	return std::make_pair(bo, pitch);
}

// ----------------------------------------------------------------
// GfxDevice::Configuration.
// ----------------------------------------------------------------

bool GfxDevice::Configuration::capture(std::vector<drm_core::Assignment> assignment, std::unique_ptr<drm_core::AtomicState> &state) {
	for(auto &assign : assignment) {
		assert(assign.property->validate(assign));
		assign.property->writeToState(assign, state);
	}

	auto crtc_states = state->crtc_states();

	for(auto pair : crtc_states) {
		auto cs = pair.second;
		auto pps = state->plane(cs->crtc().lock()->primaryPlane()->id());

		if(cs->modeChanged && cs->mode != nullptr) {
			drm_mode_modeinfo mode_info;
			memcpy(&mode_info, cs->mode->data(), sizeof(drm_mode_modeinfo));
			pps->src_h = mode_info.vdisplay;
			pps->src_w = mode_info.hdisplay;

			// TODO: Check max dimensions: _state[i]->width > 1024 || _state[i]->height > 768
			if(pps->src_w <= 0 || pps->src_h <= 0) {
				return false;
			}
		}
	}

	return true;
}

void GfxDevice::Configuration::dispose() {

}

void GfxDevice::Configuration::commit(std::unique_ptr<drm_core::AtomicState> &state) {
	_dispatch(state);
}

async::detached GfxDevice::Configuration::_dispatch(std::unique_ptr<drm_core::AtomicState> &state) {
	if(!_device->_claimedDevice) {
		co_await _device->_transport->hwDevice().claimDevice();
		_device->_claimedDevice = true;
	}

	auto crtc_states = state->crtc_states();

	for(auto pair : crtc_states) {
		auto cs = pair.second;
		auto crtc = cs->crtc().lock();
		auto pps = state->plane(crtc->primaryPlane()->id());

		if(cs->mode == nullptr) {
			std::cout << "gfx/virtio: Disable scanout" << std::endl;
			co_await Cmd::setScanout(0, 0, 0, 0, _device);

			continue;
		}

		if(pps->fb != nullptr) {
			auto fb = static_pointer_cast<GfxDevice::FrameBuffer>(pps->fb);
			auto scanoutId = static_pointer_cast<GfxDevice::Plane>(pps->plane)->scanoutId();
			auto resourceId = fb->getBufferObject()->hardwareId();

			co_await fb->getBufferObject()->wait();

			spec::XferToHost2d xfer;
			memset(&xfer, 0, sizeof(spec::XferToHost2d));
			xfer.header.type = spec::cmd::xferToHost2d;
			xfer.rect.x = 0;
			xfer.rect.y = 0;
			xfer.rect.width = pps->src_w;
			xfer.rect.height = pps->src_h;
			xfer.resourceId = fb->getBufferObject()->hardwareId();

			spec::Header xfer_result;
			virtio_core::Chain xfer_chain;
			co_await virtio_core::scatterGather(virtio_core::hostToDevice,
					xfer_chain, _device->_controlQ,
					arch::dma_buffer_view{nullptr, &xfer, sizeof(spec::XferToHost2d)});
			co_await virtio_core::scatterGather(virtio_core::deviceToHost,
					xfer_chain, _device->_controlQ,
					arch::dma_buffer_view{nullptr, &xfer_result, sizeof(spec::Header)});
			co_await AwaitableRequest{_device->_controlQ, xfer_chain.front()};
			assert(xfer_result.type == spec::resp::noData);

			co_await Cmd::setScanout(pps->src_w, pps->src_h, scanoutId, resourceId, _device);
			co_await Cmd::resourceFlush(pps->src_w, pps->src_h, resourceId, _device);
		}
	}

	complete();
}

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

uint32_t GfxDevice::FrameBuffer::getWidth() {
	return _bo->getWidth();
}

uint32_t GfxDevice::FrameBuffer::getHeight() {
	return _bo->getHeight();
}

async::detached GfxDevice::FrameBuffer::_xferAndFlush() {
	spec::XferToHost2d xfer;
	memset(&xfer, 0, sizeof(spec::XferToHost2d));
	xfer.header.type = spec::cmd::xferToHost2d;
	xfer.rect.x = 0;
	xfer.rect.y = 0;
	xfer.rect.width = _bo->getWidth();
	xfer.rect.height = _bo->getHeight();
	xfer.resourceId = _bo->hardwareId();

	spec::Header xfer_result;
	virtio_core::Chain xfer_chain;
	co_await virtio_core::scatterGather(virtio_core::hostToDevice, xfer_chain, _device->_controlQ,
		arch::dma_buffer_view{nullptr, &xfer, sizeof(spec::XferToHost2d)});
	co_await virtio_core::scatterGather(virtio_core::deviceToHost, xfer_chain, _device->_controlQ,
		arch::dma_buffer_view{nullptr, &xfer_result, sizeof(spec::Header)});
	co_await AwaitableRequest{_device->_controlQ, xfer_chain.front()};

	co_await Cmd::resourceFlush(_bo->getWidth(), _bo->getHeight(), _bo->hardwareId(), _device);
}

// ----------------------------------------------------------------
// GfxDevice: Plane.
// ----------------------------------------------------------------

GfxDevice::Plane::Plane(GfxDevice *device, int id, PlaneType type)
	:drm_core::Plane { device->allocator.allocate(), type } {
	_scanoutId = id;
}

int GfxDevice::Plane::scanoutId() {
	return _scanoutId;
}

// ----------------------------------------------------------------
// GfxDevice: BufferObject.
// ----------------------------------------------------------------

GfxDevice::BufferObject::~BufferObject() {
	_memory.release();
}

std::shared_ptr<drm_core::BufferObject> GfxDevice::BufferObject::sharedBufferObject() {
	return this->shared_from_this();
}

size_t GfxDevice::BufferObject::getSize() {
	return _size;
}

uint32_t GfxDevice::BufferObject::hardwareId() {
	return _hardwareId;
}

async::result<void> GfxDevice::BufferObject::wait() {
	return async::make_result(_jump.wait());
}

std::pair<helix::BorrowedDescriptor, uint64_t> GfxDevice::BufferObject::getMemory() {
	return std::make_pair(helix::BorrowedDescriptor{_memory}, 0);
}

async::detached GfxDevice::BufferObject::_initHw() {
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

	virtio_core::Chain chain;
	co_await virtio_core::scatterGather(virtio_core::hostToDevice, chain, _device->_controlQ,
			arch::dma_buffer_view{nullptr, &buffer, sizeof(spec::Create2d)});
	co_await virtio_core::scatterGather(virtio_core::deviceToHost, chain, _device->_controlQ,
			arch::dma_buffer_view{nullptr, &result, sizeof(spec::Header)});
	co_await AwaitableRequest{_device->_controlQ, chain.front()};

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
	virtio_core::Chain attach_chain;
	co_await virtio_core::scatterGather(virtio_core::hostToDevice, attach_chain, _device->_controlQ,
			arch::dma_buffer_view{nullptr, &attachment, sizeof(spec::AttachBacking)});
	co_await virtio_core::scatterGather(virtio_core::hostToDevice, attach_chain, _device->_controlQ,
			arch::dma_buffer_view{nullptr, entries.data(), entries.size() * sizeof(spec::MemEntry)});
	co_await virtio_core::scatterGather(virtio_core::deviceToHost, attach_chain, _device->_controlQ,
			arch::dma_buffer_view{nullptr, &attach_result, sizeof(spec::Header)});
	co_await AwaitableRequest{_device->_controlQ, attach_chain.front()};
	assert(attach_result.type == spec::resp::noData);

	_jump.raise();
}

// ----------------------------------------------------------------
// Freestanding PCI discovery functions.
// ----------------------------------------------------------------

async::result<void> doBind(mbus::Entity base_entity) {
	protocols::hw::Device hw_device(co_await base_entity.bind());
	co_await hw_device.enableBusmaster();
	auto transport = co_await virtio_core::discover(std::move(hw_device),
			virtio_core::DiscoverMode::modernOnly);

	auto gfx_device = std::make_shared<GfxDevice>(std::move(transport));
	gfx_device->initialize();

	// Create an mbus object for the device.
	auto root = co_await mbus::Instance::global().getRoot();

	mbus::Properties descriptor{
		{"drvcore.mbus-parent", mbus::StringItem{std::to_string(base_entity.getId())}},
		{"unix.subsystem", mbus::StringItem{"drm"}},
		{"unix.devname", mbus::StringItem{"dri/card0"}}
	};

	auto handler = mbus::ObjectHandler{}
	.withBind([=] () -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		drm_core::serveDrmDevice(gfx_device, std::move(local_lane));

		co_return std::move(remote_lane);
	});

	co_await root.createObject("gfx_virtio", descriptor, std::move(handler));
	baseDeviceMap.insert({base_entity.getId(), gfx_device});
}

async::result<protocols::svrctl::Error> bindDevice(int64_t base_id) {
	std::cout << "gfx/virtio: Binding to device " << base_id << std::endl;
	auto base_entity = co_await mbus::Instance::global().getEntity(base_id);

	// Do not bind to devices that are already bound to this driver.
	if(baseDeviceMap.find(base_entity.getId()) != baseDeviceMap.end())
		co_return protocols::svrctl::Error::success;

	// Make sure that we only bind to supported devices.
	auto properties = co_await base_entity.getProperties();
	if(auto vendor_str = std::get_if<mbus::StringItem>(&properties["pci-vendor"]);
			!vendor_str || vendor_str->value != "1af4")
		co_return protocols::svrctl::Error::deviceNotSupported;
	if(auto device_str = std::get_if<mbus::StringItem>(&properties["pci-device"]);
			!device_str || device_str->value != "1050")
		co_return protocols::svrctl::Error::deviceNotSupported;

	co_await doBind(base_entity);
	co_return protocols::svrctl::Error::success;
}

static constexpr protocols::svrctl::ControlOperations controlOps = {
	.bind = bindDevice
};

int main() {
	std::cout << "gfx/virtio: Starting driver" << std::endl;

	async::detach(protocols::svrctl::serveControl(&controlOps));
	async::run_forever(helix::currentDispatcher);
}

