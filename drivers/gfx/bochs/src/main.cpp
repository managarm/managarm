
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

#include "bochs.hpp"
#include <fs.pb.h>

constexpr auto fileOperations = protocols::fs::FileOperations{}
	.withRead(&drm_core::File::read)
	.withAccessMemory(&drm_core::File::accessMemory)
	.withIoctl(&drm_core::File::ioctl);

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

GfxDevice::GfxDevice(helix::UniqueDescriptor video_ram, void* frame_buffer)
: _videoRam{std::move(video_ram)}, _vramAllocator{24, 12}, _frameBuffer{frame_buffer} {
	uintptr_t ports[] = { 0x01CE, 0x01CF, 0x01D0 };
	HelHandle handle;
	HEL_CHECK(helAccessIo(ports, 3, &handle));
	HEL_CHECK(helEnableIo(handle));
	
	_operational = arch::global_io;
}

COFIBER_ROUTINE(cofiber::no_future, GfxDevice::initialize(), ([=] {
	_operational.store(regs::index, (uint16_t)RegisterIndex::id);
	auto version = _operational.load(regs::data); 
	if(version < 0xB0C2) {
		std::cout << "gfx/bochs: Device version 0x" << std::hex << version << std::dec
				<< " may be unsupported!" << std::endl;
	}

	_theCrtc = std::make_shared<Crtc>(this);
	_theCrtc->setupWeakPtr(_theCrtc);
	_theEncoder = std::make_shared<Encoder>(this);
	_theEncoder->setupWeakPtr(_theEncoder);
	_theConnector = std::make_shared<Connector>(this);
	_theConnector->setupWeakPtr(_theConnector);
	_primaryPlane = std::make_shared<Plane>(this);
	_primaryPlane->setupWeakPtr(_primaryPlane);
	
	registerObject(_theCrtc.get());
	registerObject(_theEncoder.get());
	registerObject(_theConnector.get());
	registerObject(_primaryPlane.get());
	
	_theEncoder->setCurrentCrtc(_theCrtc.get());
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
}))
	
std::unique_ptr<drm_core::Configuration> GfxDevice::createConfiguration() {
	return std::make_unique<Configuration>(this);
}

std::shared_ptr<drm_core::FrameBuffer> GfxDevice::createFrameBuffer(std::shared_ptr<drm_core::BufferObject> base_bo,
		uint32_t width, uint32_t height, uint32_t format, uint32_t pitch) {
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

	auto offset = _vramAllocator.allocate(alignment + size);
	auto buffer = std::make_shared<BufferObject>(this, alignment, size,
			offset, alignment - (offset % alignment));

	auto mapping = installMapping(buffer.get());
	buffer->setupMapping(mapping);
	return std::make_pair(buffer, pitch);
}

// ----------------------------------------------------------------
// GfxDevice::Configuration.
// ----------------------------------------------------------------

bool GfxDevice::Configuration::capture(std::vector<drm_core::Assignment> assignment) {
	for(auto &assign: assignment) {
		if(assign.property == _device->srcWProperty()) {
			_width = assign.intValue;
		}else if(assign.property == _device->srcHProperty()) {
			_height = assign.intValue;
		}else if(assign.property == _device->fbIdProperty()) {
			auto fb = assign.objectValue->asFrameBuffer();
			if(!fb)
				return false;
			_fb = static_cast<GfxDevice::FrameBuffer *>(fb);
		}else if(assign.property == _device->modeIdProperty()) {
			_mode = assign.blobValue; 
			if(_mode) {
				drm_mode_modeinfo mode_info;
				memcpy(&mode_info, _mode->data(), sizeof(drm_mode_modeinfo));
				_height = mode_info.vdisplay;
				_width = mode_info.hdisplay;
			}
		}else{
			return false;
		}
	}

		
	if(_mode) {
		if(_width <= 0 || _height <= 0 || _width > 1024 || _height > 768)
			return false;
		if(!_fb)
			return false;
	}
	return true;	
}

void GfxDevice::Configuration::dispose() {

}

void GfxDevice::Configuration::commit() {
	drm_mode_modeinfo last_mode;
	memset(&last_mode, 0, sizeof(drm_mode_modeinfo));

	if(_device->_theCrtc->currentMode())
		memcpy(&last_mode, _device->_theCrtc->currentMode()->data(), sizeof(drm_mode_modeinfo));
	
	auto switch_mode = last_mode.hdisplay != _width || last_mode.vdisplay != _height;

	_device->_theCrtc->setCurrentMode(_mode);

	if(_mode) {
		if(switch_mode) {
			// The resolution registers must be written while the device is disabled.
			_device->_operational.store(regs::index, (uint16_t)RegisterIndex::enable);
			_device->_operational.store(regs::data, enable_bits::noMemClear | enable_bits::lfb);

			_device->_operational.store(regs::index, (uint16_t)RegisterIndex::resX);
			_device->_operational.store(regs::data, _width);
			_device->_operational.store(regs::index, (uint16_t)RegisterIndex::resY);
			_device->_operational.store(regs::data, _height);
			_device->_operational.store(regs::index, (uint16_t)RegisterIndex::bpp);
			_device->_operational.store(regs::data, 32);
			
			_device->_operational.store(regs::index, (uint16_t)RegisterIndex::enable);
			_device->_operational.store(regs::data, enable_bits::enable 
					| enable_bits::noMemClear | enable_bits::lfb);
			
		}

		// We do not have to write the virtual height.
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::virtWidth);
		_device->_operational.store(regs::data, _fb->getPixelPitch());

		// The offset registers have to be written while the device is enabled!
		assert(!(_fb->getBufferObject()->getAddress() % (_fb->getPixelPitch() * 4)));
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::offX);
		_device->_operational.store(regs::data, 0);	
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::offY);
		_device->_operational.store(regs::data, _fb->getBufferObject()->getAddress()
				/ (_fb->getPixelPitch() * 4));
	}else{
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::enable);
		_device->_operational.store(regs::data, enable_bits::noMemClear | enable_bits::lfb);
	}
}

// ----------------------------------------------------------------
// GfxDevice::Connector.
// ----------------------------------------------------------------

GfxDevice::Connector::Connector(GfxDevice *device)
	: drm_core::Connector { device->allocator.allocate() } {
	_encoders.push_back(device->_theEncoder.get());
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

GfxDevice::Crtc::Crtc(GfxDevice *device)
	:drm_core::Crtc { device->allocator.allocate() } {
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
: drm_core::FrameBuffer { device->allocator.allocate() } {
	_bo = bo;
	_pixelPitch = pixel_pitch;
}

GfxDevice::BufferObject *GfxDevice::FrameBuffer::getBufferObject() {
	return _bo.get();
}

uint32_t GfxDevice::FrameBuffer::getPixelPitch() {
	return _pixelPitch;
}

void GfxDevice::FrameBuffer::notifyDirty() {
	
}

// ----------------------------------------------------------------
// GfxDevice: Plane.
// ----------------------------------------------------------------

GfxDevice::Plane::Plane(GfxDevice *device)
	:drm_core::Plane { device->allocator.allocate() } {
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
	
std::pair<helix::BorrowedDescriptor, uint64_t> GfxDevice::BufferObject::getMemory() {
	return std::make_pair(helix::BorrowedDescriptor(_device->_videoRam),
			_offset + _displacement);
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

COFIBER_ROUTINE(cofiber::no_future, bindController(mbus::Entity entity), ([=] {
	protocols::hw::Device pci_device(COFIBER_AWAIT entity.bind());
	auto info = COFIBER_AWAIT pci_device.getPciInfo();
	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypeMemory);
	auto bar = COFIBER_AWAIT pci_device.accessBar(0);
	
	void *actual_pointer;
	HEL_CHECK(helMapMemory(bar.getHandle(), kHelNullHandle, nullptr,
			0, info.barInfo[0].length, kHelMapReadWrite | kHelMapShareAtFork, &actual_pointer));

	auto gfx_device = std::make_shared<GfxDevice>(std::move(bar), actual_pointer);
	gfx_device->initialize();

	// Create an mbus object for the device.
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();
	
	mbus::Properties descriptor{
		{"unix.devtype", mbus::StringItem{"block"}},
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

	COFIBER_AWAIT root.createObject("gfx_bochs", descriptor, std::move(handler));
}))

COFIBER_ROUTINE(cofiber::no_future, observeControllers(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-vendor", "1234")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) {
		std::cout << "gfx/bochs: Detected device" << std::endl;
		bindController(std::move(entity));
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
}))

int main() {
	printf("gfx/bochs: Starting driver\n");
	
	observeControllers();

	while(true)
		helix::Dispatcher::global().dispatch();
	
	return 0;
}

