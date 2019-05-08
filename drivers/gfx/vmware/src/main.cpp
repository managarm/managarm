
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
#include <helix/memory.hpp>
#include <helix/await.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <core/drm/core.hpp>

#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>

#include "vmware.hpp"

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
// GfxDevice
// ----------------------------------------------------------------

uint32_t GfxDevice::readRegister(register_index reg) {
	_operational.store(ports::register_port, (uint32_t)reg);
	return _operational.load(ports::value_port);
}

void GfxDevice::writeRegister(register_index reg, uint32_t val) {
	_operational.store(ports::register_port, (uint32_t)reg);
	_operational.store(ports::value_port, val);
}

GfxDevice::GfxDevice(protocols::hw::Device hw_dev,
			helix::Mapping fb_mapping,
			helix::UniqueDescriptor io_bar, uint16_t io_base)
		: _hwDev(std::move(hw_dev)), _fbMapping{std::move(fb_mapping)},
		_isClaimed{false}, _deviceVersion(0) {
	HEL_CHECK(helEnableIo(io_bar.getHandle()));
	_operational = arch::io_space{io_base};
}

COFIBER_ROUTINE(cofiber::no_future, GfxDevice::initialize(), ([=] {
	auto pci_info = COFIBER_AWAIT _hwDev.getPciInfo();

	// negotiate version
	_deviceVersion = versions::id_2;

	do {
		writeRegister(register_index::id, _deviceVersion);
		if (readRegister(register_index::id) == _deviceVersion) {
			break;
		}

		_deviceVersion--;
	} while (_deviceVersion >= versions::id_0);

	assert(_deviceVersion >= versions::id_0 && "failed to negotiate version with device");
	writeRegister(register_index::config_done, 1);

	auto current_w = readRegister(register_index::width);
	auto current_h = readRegister(register_index::height);

	// device should be working from this point forward

	_crtc = std::make_shared<Crtc>(this);
	_crtc->setupWeakPtr(_crtc);
	_encoder = std::make_shared<Encoder>(this);
	_encoder->setupWeakPtr(_encoder);
	_connector = std::make_shared<Connector>(this);
	_connector->setupWeakPtr(_connector);
	_primaryPlane = std::make_shared<Plane>(this);
	_primaryPlane->setupWeakPtr(_primaryPlane);

	registerObject(_crtc.get());
	registerObject(_encoder.get());
	registerObject(_connector.get());
	registerObject(_primaryPlane.get());

	_encoder->setCurrentCrtc(_crtc.get());
	_connector->setupPossibleEncoders({_encoder.get()});
	_connector->setCurrentEncoder(_encoder.get());
	_connector->setCurrentStatus(1);
	_encoder->setupPossibleCrtcs({_crtc.get()});
	_encoder->setupPossibleClones({_encoder.get()});

	setupCrtc(_crtc.get());
	setupEncoder(_encoder.get());
	attachConnector(_connector.get());

	std::vector<drm_mode_modeinfo> supported_modes;
	drm_core::addDmtModes(supported_modes, current_w, current_h);

	std::reverse(std::begin(supported_modes), std::end(supported_modes));

	_connector->setModeList(supported_modes);

	setupMinDimensions(640, 480);
	setupMaxDimensions(current_w, current_h);

	_connector->setupPhysicalDimensions(306, 230);
	_connector->setupSubpixel(0);
}))

std::shared_ptr<drm_core::FrameBuffer> GfxDevice::createFrameBuffer(std::shared_ptr<drm_core::BufferObject> base_bo,
		uint32_t w, uint32_t h, uint32_t fmt, uint32_t pitch) {
	auto bo = std::static_pointer_cast<GfxDevice::BufferObject>(base_bo);

	auto fb = std::make_shared<FrameBuffer>(this, bo, w * 4);
	fb->setupWeakPtr(fb);
	registerObject(fb.get());
	return fb;
}

std::tuple<int, int, int> GfxDevice::driverVersion() {
	return {1, 0, 0};
}

std::tuple<std::string, std::string, std::string> GfxDevice::driverInfo() {
	return {"vmware-drm", "vmware svga interface", "20190505"};
}

std::pair<std::shared_ptr<drm_core::BufferObject>, uint32_t> GfxDevice::createDumb(uint32_t w, uint32_t h, uint32_t bpp) {
	auto size = ((w * h * bpp / 8) + 4095) & ~4095;

	HelHandle handle;
	HEL_CHECK(helAllocateMemory(size, 0, &handle));

	auto bo = std::make_shared<GfxDevice::BufferObject>(this, size, helix::UniqueDescriptor(handle));

	auto mapping = installMapping(bo.get());
	bo->setupMapping(mapping);

	return std::make_pair(bo, w * bpp / 8);
}

std::unique_ptr<drm_core::Configuration> GfxDevice::createConfiguration() {
	return std::make_unique<Configuration>(this);
}

// ----------------------------------------------------------------
// GfxDevice::Configuration
// ----------------------------------------------------------------

bool GfxDevice::Configuration::capture(std::vector<drm_core::Assignment> assignment) {
	drm_mode_modeinfo current_mode;
	memset(&current_mode, 0, sizeof(drm_mode_modeinfo));
	if (_device->_crtc->currentMode())
		memcpy(&current_mode, _device->_crtc->currentMode()->data(), sizeof(drm_mode_modeinfo));

	_width = current_mode.hdisplay;
	_height = current_mode.vdisplay;
	_mode = _device->_crtc->currentMode();

	for (auto &assign : assignment) {
		if (assign.property == _device->srcWProperty()) {
			assert(assign.property->validate(assign));
			_width = assign.intValue;
		} else if (assign.property == _device->srcHProperty()) {
			assert(assign.property->validate(assign));
			_height = assign.intValue;
		} else if (assign.property == _device->fbIdProperty()) {
			assert(assign.property->validate(assign));
			auto fb = assign.objectValue->asFrameBuffer();
			_fb = static_cast<GfxDevice::FrameBuffer *>(fb);
		} else if (assign.property == _device->modeIdProperty()) {
			assert(assign.property->validate(assign));
			_mode = assign.blobValue;
			if (_mode) {
				drm_mode_modeinfo mode_info;
				memcpy(&mode_info, _mode->data(), sizeof(drm_mode_modeinfo));
				_height = mode_info.vdisplay;
				_width = mode_info.hdisplay;
			}
		} else {
			return false;
		}
	}

	if (_mode) {
		if (_width <= 0 || _height <= 0 || _width > 1024 || _height > 768)
			return false;
		if (!_fb)
			return false;
	}

	return true;
}

void GfxDevice::Configuration::dispose() {

}

void GfxDevice::Configuration::commit() {
	commitConfiguration();
}

COFIBER_ROUTINE(cofiber::no_future, GfxDevice::Configuration::commitConfiguration(), ([&] {
	drm_mode_modeinfo last_mode;
	memset(&last_mode, 0, sizeof(drm_mode_modeinfo));
	if (_device->_crtc->currentMode())
		memcpy(&last_mode, _device->_crtc->currentMode()->data(), sizeof(drm_mode_modeinfo));

	auto switch_mode = last_mode.hdisplay != _width || last_mode.vdisplay != _height;

	_device->_crtc->setCurrentMode(_mode);

	if (_mode) {
		if (!_device->_isClaimed) {
			COFIBER_AWAIT _device->_hwDev.claimDevice();
			_device->_isClaimed = true;
			_device->writeRegister(register_index::enable, 1); // lazy init
		}

		if (switch_mode) {
			_device->writeRegister(register_index::enable, 0); // prevent weird inbetween modes
			_device->writeRegister(register_index::width, _width);
			_device->writeRegister(register_index::height, _height);
			_device->writeRegister(register_index::bits_per_pixel, 32);
			_device->writeRegister(register_index::enable, 1);
		}
	}

	if (_fb) {
		helix::Mapping user_fb{_fb->getBufferObject()->getMemory().first, 0, _fb->getBufferObject()->getSize()};
		drm_core::fastCopy16(_device->_fbMapping.get(), user_fb.get(), _fb->getBufferObject()->getSize());
		_device->writeRegister(register_index::enable, 1); // HACK: make the emulator actually redraw the window, might be a qemu issue, check in vmware
	}

	complete();
}))

// ----------------------------------------------------------------
// GfxDevice::Connector
// ----------------------------------------------------------------

GfxDevice::Connector::Connector(GfxDevice *dev)
	: drm_core::Connector { dev->allocator.allocate() } {
	_encoders.push_back(dev->_encoder.get());
}

// ----------------------------------------------------------------
// GfxDevice::Encoder
// ----------------------------------------------------------------

GfxDevice::Encoder::Encoder(GfxDevice *dev)
	: drm_core::Encoder { dev->allocator.allocate() } {
}

// ----------------------------------------------------------------
// GfxDevice::Crtc
// ----------------------------------------------------------------

GfxDevice::Crtc::Crtc(GfxDevice *dev)
	: drm_core::Crtc { dev->allocator.allocate() } {
	_device = dev;
}

drm_core::Plane *GfxDevice::Crtc::Crtc::primaryPlane() {
	return _device->_primaryPlane.get();
}

// ----------------------------------------------------------------
// GfxDevice::FrameBuffer
// ----------------------------------------------------------------

GfxDevice::FrameBuffer::FrameBuffer(GfxDevice *dev,
		std::shared_ptr<GfxDevice::BufferObject> bo, uint32_t pixel_pitch)
	: drm_core::FrameBuffer { dev->allocator.allocate() } {
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
// GfxDevice::Plane
// ----------------------------------------------------------------

GfxDevice::Plane::Plane(GfxDevice *dev)
	:drm_core::Plane { dev->allocator.allocate() } {
}

// ----------------------------------------------------------------
// GfxDevice::BufferObject
// ----------------------------------------------------------------

GfxDevice::BufferObject::BufferObject(GfxDevice *dev, size_t size, helix::UniqueDescriptor mem)
	: _device{dev}, _size{size}, _mem{std::move(mem)} {
}

std::shared_ptr<drm_core::BufferObject> GfxDevice::BufferObject::sharedBufferObject() {
	return this->shared_from_this();
}

size_t GfxDevice::BufferObject::getSize() {
	return _size;
}

std::pair<helix::BorrowedDescriptor, uint64_t> GfxDevice::BufferObject::getMemory() {
	return std::make_pair(helix::BorrowedDescriptor{_mem}, 0);
}

// ----------------------------------------------------------------

COFIBER_ROUTINE(cofiber::no_future, setupDevice(mbus::Entity entity), ([=] {
	std::cout << "gfx/vmware: setting up the device" << std::endl;

	protocols::hw::Device pci_device(COFIBER_AWAIT entity.bind());
	auto info = COFIBER_AWAIT pci_device.getPciInfo();

	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypePort);
	assert(info.barInfo[1].ioType == protocols::hw::IoType::kIoTypeMemory);
	assert(info.barInfo[2].ioType == protocols::hw::IoType::kIoTypeMemory);

	auto io_bar = COFIBER_AWAIT pci_device.accessBar(0);
	auto fb_bar = COFIBER_AWAIT pci_device.accessBar(1);

	auto io_bar_info = info.barInfo[0];
	auto fb_bar_info = info.barInfo[1];

	helix::Mapping fb_mapping(fb_bar, 0, fb_bar_info.length);

	auto gfx_device = std::make_shared<GfxDevice>(std::move(pci_device),
			helix::Mapping{fb_bar, 0, fb_bar_info.length},
			std::move(io_bar), io_bar_info.address);

	gfx_device->initialize();

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
	COFIBER_AWAIT root.createObject("gfx_vmware", descriptor, std::move(handler));

}))

COFIBER_ROUTINE(cofiber::no_future, findDevice(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-vendor", "15ad"),
		mbus::EqualsFilter("pci-device", "0405")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties) {
		printf("gfx/vmware: found a vmware svga device\n");
		setupDevice(std::move(entity));
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
}))

int main() {
	printf("gfx/vmware: starting driver\n");

	{
		async::queue_scope scope{helix::globalQueue()};
		findDevice();
	}

	helix::globalQueue()->run();

	return 0;
}

