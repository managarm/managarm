
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
#include <arch/io_space.hpp>
#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <core/drm/core.hpp>

#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>

#include "bochs.hpp"
#include <fs.bragi.hpp>

namespace {
	constexpr bool logBuffers = false;
	constexpr bool logCommits = false;
}

// ----------------------------------------------------------------
// GfxDevice.
// ----------------------------------------------------------------

GfxDevice::GfxDevice(protocols::hw::Device hw_device,
		helix::UniqueDescriptor video_ram, void *)
: _videoRam{std::move(video_ram)}, _hwDevice{std::move(hw_device)},
		_vramAllocator{24, 12}, _claimedDevice{false} {
	uintptr_t ports[] = { 0x01CE, 0x01CF, 0x01D0 };
	HelHandle handle;
	HEL_CHECK(helAccessIo(ports, 3, &handle));
	HEL_CHECK(helEnableIo(handle));

	_operational = arch::global_io;
}

async::detached GfxDevice::initialize() {
	std::vector<drm_core::Assignment> assignments;

	_operational.store(regs::index, (uint16_t)RegisterIndex::id);
	auto version = _operational.load(regs::data);
	if(version < 0xB0C2) {
		std::cout << "gfx/bochs: Device version 0x" << std::hex << version << std::dec
				<< " may be unsupported!" << std::endl;
	}

	_theCrtc = std::make_shared<Crtc>(this);
	_theCrtc->setupWeakPtr(_theCrtc);
	_theCrtc->setupState(_theCrtc);
	_theEncoder = std::make_shared<Encoder>(this);
	_theEncoder->setupWeakPtr(_theEncoder);
	_theConnector = std::make_shared<Connector>(this);
	_theConnector->setupWeakPtr(_theConnector);
	_theConnector->setupState(_theConnector);
	_primaryPlane = std::make_shared<Plane>(this, Plane::PlaneType::PRIMARY);
	_primaryPlane->setupWeakPtr(_primaryPlane);
	_primaryPlane->setupState(_primaryPlane);

	registerObject(_theCrtc.get());
	registerObject(_theEncoder.get());
	registerObject(_theConnector.get());
	registerObject(_primaryPlane.get());

	assignments.push_back(drm_core::Assignment::withInt(_theCrtc, activeProperty(), 0));

	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, planeTypeProperty(), 1));
	assignments.push_back(drm_core::Assignment::withModeObj(_primaryPlane, crtcIdProperty(), _theCrtc));
	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, srcHProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, srcWProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, crtcHProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, crtcWProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, srcXProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, srcYProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, crtcXProperty(), 0));
	assignments.push_back(drm_core::Assignment::withInt(_primaryPlane, crtcYProperty(), 0));
	assignments.push_back(drm_core::Assignment::withModeObj(_primaryPlane, fbIdProperty(), nullptr));

	assignments.push_back(drm_core::Assignment::withInt(_theConnector, dpmsProperty(), 3));
	assignments.push_back(drm_core::Assignment::withModeObj(_theConnector, crtcIdProperty(), _theCrtc));

	_theEncoder->setCurrentCrtc(_theCrtc.get());
	_theConnector->setupPossibleEncoders({_theEncoder.get()});
	_theConnector->setCurrentEncoder(_theEncoder.get());
	_theConnector->setCurrentStatus(1);
	_theEncoder->setupPossibleCrtcs({_theCrtc.get()});
	_theEncoder->setupPossibleClones({_theEncoder.get()});

	setupCrtc(_theCrtc.get());
	setupEncoder(_theEncoder.get());
	attachConnector(_theConnector.get());

	std::vector<drm_mode_modeinfo> supported_modes;
	drm_core::addDmtModes(supported_modes, 1024, 768);
	_theConnector->setModeList(supported_modes);

	setupMinDimensions(640, 480);
	setupMaxDimensions(1024, 768);

	_theConnector->setupPhysicalDimensions(306, 230);
	_theConnector->setupSubpixel(0);
	_theConnector->setConnectorType(DRM_MODE_CONNECTOR_VIRTUAL);

	auto config = createConfiguration();
	auto state = atomicState();
	assert(config->capture(assignments, state));
	config->commit(state);
	co_await config->waitForCompletion();

	co_return;
}

std::unique_ptr<drm_core::Configuration> GfxDevice::createConfiguration() {
	return std::make_unique<Configuration>(this);
}

std::shared_ptr<drm_core::FrameBuffer> GfxDevice::createFrameBuffer(std::shared_ptr<drm_core::BufferObject> base_bo,
		uint32_t width, uint32_t height, uint32_t, uint32_t pitch) {
	auto bo = std::static_pointer_cast<GfxDevice::BufferObject>(base_bo);

	assert(pitch % 4 == 0);
	auto pixel_pitch = pitch / 4;

	assert(pixel_pitch >= width);
	assert(bo->getAlignment() % pitch == 0);
	assert(bo->getSize() >= pitch * height);

	auto fb = std::make_shared<FrameBuffer>(this, bo, pixel_pitch);
	fb->setupWeakPtr(fb);
	registerObject(fb.get());
	return fb;
}

std::tuple<int, int, int> GfxDevice::driverVersion() {
	return {1, 0, 0};
}

std::tuple<std::string, std::string, std::string> GfxDevice::driverInfo() {
	return {"bochs-drm", "bochs dispi vga interface (qemu stdvga)", "20130925"};
}

std::pair<std::shared_ptr<drm_core::BufferObject>, uint32_t>
GfxDevice::createDumb(uint32_t width, uint32_t height, uint32_t bpp) {
	assert(bpp == 32);
	unsigned int page_size = 4096;
	unsigned int bytes_pp = bpp / 8;

	// Buffers need to be aligned to lcm(pitch, page size). Here we compute a pitch that
	// minimizes the effective size (= data size + alignment) of the buffer.
	// avdgrinten: I'm not sure if there is a closed form expression for that, so we
	// just perform a brute-force search. Stop once the pitch is so big that no improvement
	// to the alignment can decrease the buffer size.
	auto best_ppitch = width;
	auto best_esize = std::lcm(bytes_pp * width, page_size) + bytes_pp * width * height;
	auto best_waste = std::lcm(bytes_pp * width, page_size);
	for(auto ppitch = width; bytes_pp * (ppitch - width) * height < best_waste; ++ppitch) {
		auto esize = std::lcm(bytes_pp * ppitch, page_size) + bytes_pp * ppitch * height;
		if(esize < best_esize) {
			best_ppitch = ppitch;
			best_esize = esize;
			best_waste = std::lcm(bytes_pp * best_ppitch, page_size)
					+ bytes_pp * (best_ppitch - width) * height;
		}
	}

	// TODO: Once we support VRAM <-> RAM eviction, we do not need to
	// statically determine the alignment at buffer creation time.
	auto pitch = bytes_pp * best_ppitch;
	auto alignment = std::lcm(pitch, page_size);
	auto size = ((pitch * height) + (page_size - 1)) & ~(page_size - 1);
	if(logBuffers)
		std::cout << "gfx-bochs: Preparing " << bpp << "-bpp "
				<< width << "x" << height << " buffer."
				" Computed pixel pitch: " << best_ppitch << std::endl;

	auto offset = _vramAllocator.allocate(alignment + size);
	auto displacement = alignment - (offset % alignment);
	if(logBuffers)
		std::cout << "gfx-bochs: Allocating buffer of size " << (void *)(size_t)size
				<< " at " << (void *)offset
				<< ", displacement is: " << (void *)displacement << std::endl;
	auto buffer = std::make_shared<BufferObject>(this, alignment, size,
			offset, displacement, width, height);

	auto mapping = installMapping(buffer.get());
	buffer->setupMapping(mapping);
	return std::make_pair(buffer, pitch);
}

// ----------------------------------------------------------------
// GfxDevice::Configuration.
// ----------------------------------------------------------------

bool GfxDevice::Configuration::capture(std::vector<drm_core::Assignment> assignment, std::unique_ptr<drm_core::AtomicState> &state) {
	for(auto &assign: assignment) {
		assert(assign.property->validate(assign));
		assign.property->writeToState(assign, state);
	}

	auto plane_state = state->plane(_device->_primaryPlane->id());
	auto crtc_state = state->crtc(_device->_theCrtc->id());

	if(crtc_state->mode != nullptr) {
		// TODO: Consider current width/height if FB did not change.
		drm_mode_modeinfo mode_info;
		memcpy(&mode_info, crtc_state->mode->data(), sizeof(drm_mode_modeinfo));
		plane_state->src_h = mode_info.vdisplay;
		plane_state->src_w = mode_info.hdisplay;

		// TODO: Check max dimensions: plane_state->width > 1024 || plane_state->height > 768
		if(plane_state->src_w <= 0 || plane_state->src_h <= 0) {
			std::cout << "\e[31m" "gfx/bochs: invalid state width of height" << "\e[39m" << std::endl;
			return false;
		}
	}

	return true;
}

void GfxDevice::Configuration::dispose() {

}

void GfxDevice::Configuration::commit(std::unique_ptr<drm_core::AtomicState> &state) {
	_doCommit(state);
}

async::detached GfxDevice::Configuration::_doCommit(std::unique_ptr<drm_core::AtomicState> &state) {
	if(logCommits)
		std::cout << "gfx-bochs: Committing configuration" << std::endl;

	auto primary_plane_state = state->plane(_device->_primaryPlane->id());
	auto crtc_state = state->crtc(_device->_theCrtc->id());

	drm_mode_modeinfo last_mode;
	memset(&last_mode, 0, sizeof(drm_mode_modeinfo));
	if(_device->_theCrtc->drmState()->mode)
		memcpy(&last_mode, _device->_theCrtc->drmState()->mode->data(), sizeof(drm_mode_modeinfo));

	auto switch_mode = last_mode.hdisplay != primary_plane_state->src_w || last_mode.vdisplay != primary_plane_state->src_h;

	if(crtc_state->mode != nullptr) {
		if(!_device->_claimedDevice) {
			co_await _device->_hwDevice.claimDevice();
			_device->_claimedDevice = true;
		}

		if(switch_mode) {
			// The resolution registers must be written while the device is disabled.
			_device->_operational.store(regs::index, (uint16_t)RegisterIndex::enable);
			_device->_operational.store(regs::data, enable_bits::noMemClear | enable_bits::lfb);

			_device->_operational.store(regs::index, (uint16_t)RegisterIndex::resX);
			_device->_operational.store(regs::data, primary_plane_state->src_w);
			_device->_operational.store(regs::index, (uint16_t)RegisterIndex::resY);
			_device->_operational.store(regs::data, primary_plane_state->src_h);
			_device->_operational.store(regs::index, (uint16_t)RegisterIndex::bpp);
			_device->_operational.store(regs::data, 32);

			_device->_operational.store(regs::index, (uint16_t)RegisterIndex::enable);
			_device->_operational.store(regs::data, enable_bits::enable
					| enable_bits::noMemClear | enable_bits::lfb);

		}

		auto fb = static_pointer_cast<GfxDevice::FrameBuffer>(primary_plane_state->fb);

		// We do not have to write the virtual height.
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::virtWidth);
		_device->_operational.store(regs::data, fb->getPixelPitch());

		// The offset registers have to be written while the device is enabled!
		assert(!(fb->getBufferObject()->getAddress() % (fb->getPixelPitch() * 4)));
		if(logCommits)
			std::cout << "gfx-bochs: Flip to buffer at "
					<< (void *)fb->getBufferObject()->getAddress() << std::endl;
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::offX);
		_device->_operational.store(regs::data, 0);
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::offY);
		_device->_operational.store(regs::data, fb->getBufferObject()->getAddress()
				/ (fb->getPixelPitch() * 4));
	}else{
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::enable);
		_device->_operational.store(regs::data, enable_bits::noMemClear | enable_bits::lfb);
	}

	complete();
}

// ----------------------------------------------------------------
// GfxDevice::Connector.
// ----------------------------------------------------------------

GfxDevice::Connector::Connector(GfxDevice *device)
	: drm_core::Connector { device, device->allocator.allocate() } {
	_encoders.push_back(device->_theEncoder.get());
}

// ----------------------------------------------------------------
// GfxDevice::Encoder.
// ----------------------------------------------------------------

GfxDevice::Encoder::Encoder(GfxDevice *device)
	:drm_core::Encoder { device, device->allocator.allocate() } {
}

// ----------------------------------------------------------------
// GfxDevice::Crtc.
// ----------------------------------------------------------------

GfxDevice::Crtc::Crtc(GfxDevice *device)
	:drm_core::Crtc { device, device->allocator.allocate() } {
	_device = device;
}

drm_core::Plane *GfxDevice::Crtc::primaryPlane() {
	return _device->_primaryPlane.get();
}

// ----------------------------------------------------------------
// GfxDevice::FrameBuffer.
// ----------------------------------------------------------------

GfxDevice::FrameBuffer::FrameBuffer(GfxDevice *device,
		std::shared_ptr<GfxDevice::BufferObject> bo, uint32_t pixel_pitch)
: drm_core::FrameBuffer { device, device->allocator.allocate() } {
	_bo = bo;
	_pixelPitch = pixel_pitch;
}

GfxDevice::BufferObject *GfxDevice::FrameBuffer::getBufferObject() {
	return _bo.get();
}

uint32_t GfxDevice::FrameBuffer::getPixelPitch() {
	return _pixelPitch;
}

uint32_t GfxDevice::FrameBuffer::getWidth() {
	return _bo->getWidth();
}

uint32_t GfxDevice::FrameBuffer::getHeight() {
	return _bo->getHeight();
}

void GfxDevice::FrameBuffer::notifyDirty() {

}

// ----------------------------------------------------------------
// GfxDevice: Plane.
// ----------------------------------------------------------------

GfxDevice::Plane::Plane(GfxDevice *device, PlaneType type)
	:drm_core::Plane { device, device->allocator.allocate(), type } {
}

// ----------------------------------------------------------------
// GfxDevice: BufferObject.
// ----------------------------------------------------------------

GfxDevice::BufferObject::BufferObject(GfxDevice *device, size_t alignment, size_t size,
		uintptr_t offset, ptrdiff_t displacement, uint32_t width, uint32_t height)
: drm_core::BufferObject{width, height}, _device{device}, _alignment{alignment}, _size{size},
		_offset{offset}, _displacement{displacement} {
	assert(!((_offset + _displacement) % 0x1000));
	assert(!((_offset + _displacement) % _alignment));

	HelHandle handle;
	HEL_CHECK(helCreateSliceView(_device->_videoRam.getHandle(),
			_offset + _displacement, _size, 0, &handle));
	_memoryView = helix::UniqueDescriptor{handle};
};

std::shared_ptr<drm_core::BufferObject> GfxDevice::BufferObject::sharedBufferObject() {
	return this->shared_from_this();
}

size_t GfxDevice::BufferObject::getSize() {
	return _size;
}

std::pair<helix::BorrowedDescriptor, uint64_t> GfxDevice::BufferObject::getMemory() {
	return std::make_pair(helix::BorrowedDescriptor{_memoryView}, 0);
}

size_t GfxDevice::BufferObject::getAlignment() {
	return _alignment;
}

uintptr_t GfxDevice::BufferObject::getAddress() {
	return _offset + _displacement;
}

// ----------------------------------------------------------------
// Freestanding PCI discovery functions.
// ----------------------------------------------------------------

async::detached bindController(mbus::Entity entity) {
	protocols::hw::Device pci_device(co_await entity.bind());
	auto info = co_await pci_device.getPciInfo();
	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypeMemory);
	auto bar = co_await pci_device.accessBar(0);

	void *actual_pointer;
	HEL_CHECK(helMapMemory(bar.getHandle(), kHelNullHandle, nullptr,
			0, info.barInfo[0].length, kHelMapProtRead | kHelMapProtWrite,
			&actual_pointer));

	auto gfx_device = std::make_shared<GfxDevice>(std::move(pci_device),
			std::move(bar), actual_pointer);
	gfx_device->initialize();

	// Create an mbus object for the device.
	auto root = co_await mbus::Instance::global().getRoot();

	mbus::Properties descriptor{
		{"drvcore.mbus-parent", mbus::StringItem{std::to_string(entity.getId())}},
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

	co_await root.createObject("gfx_bochs", descriptor, std::move(handler));
}

async::detached observeControllers() {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-vendor", "1234")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties) {
		std::cout << "gfx/bochs: Detected device" << std::endl;
		bindController(std::move(entity));
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
}

int main() {
	printf("gfx/bochs: Starting driver\n");

	observeControllers();
	async::run_forever(helix::currentDispatcher);
}

