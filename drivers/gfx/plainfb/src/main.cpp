
#include <assert.h>
#include <immintrin.h>
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

#include "plainfb.hpp"
#include <fs.pb.h>

constexpr auto fileOperations = protocols::fs::FileOperations{}
	.withRead(&drm_core::File::read)
	.withAccessMemory(&drm_core::File::accessMemory)
	.withIoctl(&drm_core::File::ioctl)
	.withPoll(&drm_core::File::poll);

COFIBER_ROUTINE(cofiber::no_future, serveDevice(std::shared_ptr<drm_core::Device> device,
		helix::UniqueLane p), ([device = std::move(device), lane = std::move(p)] {
	std::cout << "gfx/plainfb: Connection" << std::endl;

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
			assert(!req.flags());

			helix::SendBuffer send_resp;
			helix::PushDescriptor push_pt;
			helix::PushDescriptor push_page;
			
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			auto file = smarter::make_shared<drm_core::File>(device);
			async::detach(protocols::fs::servePassthrough(
					std::move(local_lane), file, &fileOperations));

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_caps(managarm::fs::FC_STATUS_PAGE);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&push_pt, remote_lane, kHelItemChain),
					helix::action(&push_page, file->statusPageMemory()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_pt.error());
			HEL_CHECK(push_page.error());
		}else{
			throw std::runtime_error("Invalid request in serveDevice()");
		}
	}
}))

// ----------------------------------------------------------------
// GfxDevice.
// ----------------------------------------------------------------

GfxDevice::GfxDevice(protocols::hw::Device hw_device,
		unsigned int screen_width, unsigned int screen_height,
		size_t screen_pitch, helix::Mapping fb_mapping)
: _hwDevice{std::move(hw_device)},
		_screenWidth{screen_width}, _screenHeight{screen_height},
		_screenPitch{screen_pitch},_fbMapping{std::move(fb_mapping)} {
	if((reinterpret_cast<uintptr_t>(fb_mapping.get()) & 15)) {
		std::cout << "\e[31m" "gfx/plainfb: Hardware framebuffer is not aligned;"
				" expect perfomance degradation!" "\e[39m" << std::endl;
		_hardwareFbIsAligned = false;
	}
}

COFIBER_ROUTINE(cofiber::no_future, GfxDevice::initialize(), ([=] { 
	// Setup planes, encoders and CRTCs (i.e. the static entities).
	auto plane = std::make_shared<Plane>(this);
	_theCrtc = std::make_shared<Crtc>(this, plane);
	_theEncoder = std::make_shared<Encoder>(this);
	
	plane->setupWeakPtr(plane);
	_theCrtc->setupWeakPtr(_theCrtc);
	_theEncoder->setupWeakPtr(_theEncoder);

	_theEncoder->setupPossibleCrtcs({_theCrtc.get()});
	_theEncoder->setupPossibleClones({_theEncoder.get()});
	_theEncoder->setCurrentCrtc(_theCrtc.get());

	registerObject(plane.get());
	registerObject(_theCrtc.get());
	registerObject(_theEncoder.get());

	setupCrtc(_theCrtc.get());
	setupEncoder(_theEncoder.get());

	// Setup the connector.
	_theConnector = std::make_shared<Connector>(this);
	_theConnector->setupWeakPtr(_theConnector);

	_theConnector->setupPossibleEncoders({_theEncoder.get()});
	_theConnector->setCurrentEncoder(_theEncoder.get());
	_theConnector->setCurrentStatus(1);
	
	registerObject(_theConnector.get());
	attachConnector(_theConnector.get());
	
	std::vector<drm_mode_modeinfo> supported_modes;
	drm_core::addDmtModes(supported_modes, _screenWidth, _screenHeight);
	std::sort(supported_modes.begin(), supported_modes.end(),
			[] (const drm_mode_modeinfo &u, const drm_mode_modeinfo &v) {
		return u.hdisplay * u.vdisplay > v.hdisplay * v.vdisplay;
	});
	_theConnector->setModeList(supported_modes);
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

	auto fb = std::make_shared<FrameBuffer>(this, bo, pitch);
	fb->setupWeakPtr(fb);
	registerObject(fb.get());
	return fb;
}

std::tuple<int, int, int> GfxDevice::driverVersion() {
	return {0, 0, 1};
}

std::tuple<std::string, std::string, std::string> GfxDevice::driverInfo() {
	return {"plainfb_gpu", "plainfb gpu", "0"};
}

std::pair<std::shared_ptr<drm_core::BufferObject>, uint32_t>
GfxDevice::createDumb(uint32_t width, uint32_t height, uint32_t bpp) {
	HelHandle handle;
	auto size = ((width * height * bpp / 8) + (4096 - 1)) & ~(4096 - 1);
	HEL_CHECK(helAllocateMemory(size, 0, &handle));

	auto bo = std::make_shared<BufferObject>(this, size,
			helix::UniqueDescriptor(handle), width, height);
	uint32_t pitch = width * bpp / 8;
	
	auto mapping = installMapping(bo.get());
	bo->setupMapping(mapping);
	
	return std::make_pair(bo, pitch);
}

// ----------------------------------------------------------------
// GfxDevice::Configuration.
// ----------------------------------------------------------------

bool GfxDevice::Configuration::capture(std::vector<drm_core::Assignment> assignment) {
	auto captureScanout = [&] () {
		if(_state)
			return;

		_state.emplace();
		// TODO: Capture FB, width and height.
		_state->mode = _device->_theCrtc->currentMode();
	};

	for(auto &assign : assignment) {
		if(assign.property == _device->srcWProperty()) {
			//TODO: check this outside of capure
			assert(assign.property->validate(assign));
			
			captureScanout();
			_state->width = assign.intValue;
		}else if(assign.property == _device->srcHProperty()) {
			//TODO: check this outside of capure
			assert(assign.property->validate(assign));

			captureScanout();
			_state->height = assign.intValue;
		}else if(assign.property == _device->fbIdProperty()) {
			//TODO: check this outside of capure
			assert(assign.property->validate(assign));

			auto fb = assign.objectValue->asFrameBuffer();
			captureScanout();
			_state->fb = static_cast<GfxDevice::FrameBuffer *>(fb);
		}else if(assign.property == _device->modeIdProperty()) {
			//TODO: check this outside of capture.
			assert(assign.property->validate(assign));
		
			captureScanout();
			_state->mode = assign.blobValue;
		}else{
			return false;
		}
	}

	if(_state && _state->mode) {
		// TODO: Consider current width/height if FB did not change.
		drm_mode_modeinfo mode_info;
		memcpy(&mode_info, _state->mode->data(), sizeof(drm_mode_modeinfo));
		_state->height = mode_info.vdisplay;
		_state->width = mode_info.hdisplay;
		
		// TODO: Check max dimensions: _state->width > 1024 || _state->height > 768
		if(_state->width <= 0 || _state->height <= 0)
			return false;
		if(!_state->fb)
			return false;
	}
	return true;
}

void GfxDevice::Configuration::dispose() {

}

void GfxDevice::Configuration::commit() {
	if(_state)
		_device->_theCrtc->setCurrentMode(_state->mode);

	_dispatch();
}

COFIBER_ROUTINE(cofiber::no_future, GfxDevice::Configuration::_dispatch(), ([=] {
	if(_state && _state->mode) {
//		std::cout << "Swap to framebuffer " << _state[i]->fb->id()
//				<< " " << _state[i]->width << "x" << _state[i]->height << std::endl;

		if(!_device->_claimedDevice) {
			COFIBER_AWAIT _device->_hwDevice.claimDevice();
			_device->_claimedDevice = true;
		}

		auto bo = _state->fb->getBufferObject();
		assert(bo->getWidth() == _device->_screenWidth);
		assert(bo->getHeight() == _device->_screenHeight);
		auto dest = reinterpret_cast<char *>(_device->_fbMapping.get());
		auto src = reinterpret_cast<char *>(bo->accessMapping());

		if(_state->fb->fastScanout()) {
			for(unsigned int k = 0; k < bo->getHeight(); k++) {
				drm_core::fastCopy16(dest, src, bo->getWidth() * 4);
				dest += _device->_screenPitch;
				src += _state->fb->getPitch();
			}
		}else{
			for(unsigned int k = 0; k < bo->getHeight(); k++) {
				memcpy(dest, src, bo->getWidth() * 4);
				dest += _device->_screenPitch;
				src += _state->fb->getPitch();
			}
		}
	}else if(_state) {
		assert(!_state->mode);
		std::cout << "gfx/plainfb: Disable scanout" << std::endl;
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

GfxDevice::Crtc::Crtc(GfxDevice *device, std::shared_ptr<Plane> plane)
: drm_core::Crtc{device->allocator.allocate()}, _device{device},
		_primaryPlane{std::move(plane)} { }

drm_core::Plane *GfxDevice::Crtc::primaryPlane() {
	return _primaryPlane.get();
}

// ----------------------------------------------------------------
// GfxDevice::FrameBuffer.
// ----------------------------------------------------------------

GfxDevice::FrameBuffer::FrameBuffer(GfxDevice *device,
		std::shared_ptr<GfxDevice::BufferObject> bo, size_t pitch)
: drm_core::FrameBuffer{device->allocator.allocate()},
		_device{device}, _bo{std::move(bo)}, _pitch{pitch} {
	if(!_device->_hardwareFbIsAligned) {
		_fastScanout = false;
	}else if((reinterpret_cast<uintptr_t>(_bo->accessMapping()) & 15)
			|| ((_bo->getWidth() * 4) & 15)) {
		std::cout << "\e[31m" "gfx/plainfb: Framebuffer is not aligned!" "\e[39m" << std::endl;
		_fastScanout = false;
	}
}

size_t GfxDevice::FrameBuffer::getPitch() {
	return _pitch;
}

GfxDevice::BufferObject *GfxDevice::FrameBuffer::getBufferObject() {
	return _bo.get();
}

void GfxDevice::FrameBuffer::notifyDirty() {
	// TODO: Re-blit the FrameBuffer if it is currently displayed.
	std::cout << "gfx/plainfb: notifyDirty() is not implemented correctly" << std::endl;
}

// ----------------------------------------------------------------
// GfxDevice: Plane.
// ----------------------------------------------------------------

GfxDevice::Plane::Plane(GfxDevice *device)
: drm_core::Plane{device->allocator.allocate()} { }

// ----------------------------------------------------------------
// GfxDevice: BufferObject.
// ----------------------------------------------------------------

GfxDevice::BufferObject::BufferObject(GfxDevice *device,
		size_t size, helix::UniqueDescriptor memory,
		uint32_t width, uint32_t height)
: _device{device}, _size{size}, _memory{std::move(memory)},
		_width{width}, _height{height} {
	_bufferMapping = helix::Mapping{_memory, 0, getSize()};
}

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

void *GfxDevice::BufferObject::accessMapping() {
	return _bufferMapping.get();
}

std::pair<helix::BorrowedDescriptor, uint64_t> GfxDevice::BufferObject::getMemory() {
	return std::make_pair(helix::BorrowedDescriptor(_memory), 0);
}

// ----------------------------------------------------------------
//
// ----------------------------------------------------------------

COFIBER_ROUTINE(cofiber::no_future, bindController(mbus::Entity entity), ([=] {
	protocols::hw::Device hw_device(COFIBER_AWAIT entity.bind());

	auto info = COFIBER_AWAIT hw_device.getFbInfo();
	auto fb_memory = COFIBER_AWAIT hw_device.accessFbMemory();
	std::cout << "gfx/plainfb: Resolution " << info.width
			<< "x" << info.height << " (" << info.bpp
			<< " bpp, pitch: " << info.pitch << ")" << std::endl;
	assert(info.bpp == 32);
	
	auto gfx_device = std::make_shared<GfxDevice>(std::move(hw_device),
			info.width, info.height, info.pitch,
			helix::Mapping{fb_memory, 0, info.pitch * info.height});
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

	COFIBER_AWAIT root.createObject("gfx_plainfb", descriptor, std::move(handler));
}))

COFIBER_ROUTINE(cofiber::no_future, observeControllers(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("class", "framebuffer")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) {
		std::cout << "gfx/plainfb: Detected device" << std::endl;
		bindController(std::move(entity));
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
}))

int main() {
	std::cout << "gfx/plainfb: Starting driver" << std::endl;

	{
		async::queue_scope scope{helix::globalQueue()};
		observeControllers();
	}

	helix::globalQueue()->run();
	
	return 0;
}

