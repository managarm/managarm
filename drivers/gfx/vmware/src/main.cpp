
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
			helix::Mapping fifo_mapping,
			helix::UniqueDescriptor io_bar, uint16_t io_base)
		: _hwDev(std::move(hw_dev)), _fifo{this, std::move(fifo_mapping)},_fbMapping{std::move(fb_mapping)}, _isClaimed{false}, _deviceVersion{0} {
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

	_deviceCaps = _deviceVersion >= versions::id_1 ? readRegister(register_index::capabilities) : 0;

	// configure fifo
	_fifo.initialize();

	if (_deviceCaps & (uint32_t)caps::irqmask) {
		writeRegister(register_index::irqmask, 0);
		_operational.store(ports::irq_status_port, 0xFF);
	} else {
		printf("\e[35mgfx/vmware: device doesn't support interrupts\e[39m\n");
	}

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
	
	if (hasCapability(caps::cursor)) {
		_cursorPlane = std::make_shared<Plane>(this);
		_cursorPlane->setupWeakPtr(_cursorPlane);
	}

	registerObject(_crtc.get());
	registerObject(_encoder.get());
	registerObject(_connector.get());
	registerObject(_primaryPlane.get());

    if (hasCapability(caps::cursor))
		registerObject(_cursorPlane.get());

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

bool GfxDevice::hasCapability(caps capability) {
	return (_deviceCaps & (uint32_t)capability) != 0;
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

COFIBER_ROUTINE(cofiber::no_future, GfxDevice::waitIrq(uint32_t irq_mask), ([=](){

	static uint64_t irq_sequence;
	
	if (_deviceCaps & (uint32_t)caps::irqmask) {
		writeRegister(register_index::irqmask, irq_mask);
		writeRegister(register_index::sync, 1);

		auto irq = COFIBER_AWAIT _hwDev.accessIrq();

		while (true) {
			helix::AwaitEvent await_irq;
			auto &&submit = helix::submitAwaitEvent(irq, &await_irq, irq_sequence,
			    helix::Dispatcher::global());
			COFIBER_AWAIT submit.async_wait();
			HEL_CHECK(await_irq.error());
			irq_sequence = await_irq.sequence();

			uint32_t irq_flags = _operational.load(ports::irq_status_port);
			if (!(irq_flags & irq_mask)) {
				HEL_CHECK(helAcknowledgeIrq(irq.getHandle(), kHelAckNack, irq_sequence));
				continue;
			}

			_operational.store(ports::irq_status_port, irq_flags);

			HEL_CHECK(helAcknowledgeIrq(irq.getHandle(), kHelAckAcknowledge, irq_sequence));
			break;
		}
	} else {
		writeRegister(register_index::sync, 1);
		readRegister(register_index::busy);
	}

}))

// ----------------------------------------------------------------
// GfxDevice::DeviceFifo
// ----------------------------------------------------------------

GfxDevice::DeviceFifo::DeviceFifo(GfxDevice *device, helix::Mapping fifoMapping)
	:_device{device}, _fifoMapping{std::move(fifoMapping)}, _fifoSize{0} {
}

void inline GfxDevice::DeviceFifo::writeRegister(fifo_index idx, uint32_t value) {
	auto mem = static_cast<volatile uint32_t *>(_fifoMapping.get());
	mem[(uint32_t)idx] = value;
}

uint32_t inline GfxDevice::DeviceFifo::readRegister(fifo_index idx) {
	auto mem = static_cast<volatile uint32_t *>(_fifoMapping.get());
	return mem[(uint32_t)idx];
}

void GfxDevice::DeviceFifo::initialize() {
	_fifoSize = _device->readRegister(register_index::mem_size);

	uint32_t min = (uint32_t)fifo_index::num_regs * 4;
	writeRegister(fifo_index::min, min);
	writeRegister(fifo_index::max, _fifoSize);
	writeRegister(fifo_index::next_cmd, min);
	writeRegister(fifo_index::stop, min);

	_device->writeRegister(register_index::config_done, 1);
}

bool GfxDevice::DeviceFifo::hasCapability(caps capability) {
	if (!_device->hasCapability(caps::fifo_extended))
		return false;
	return (readRegister(fifo_index::capabilities) & (uint32_t)capability) != 0;
}

void *GfxDevice::DeviceFifo::reserve(size_t bytes) {
	bytes *= 4;
	uint32_t min = readRegister(fifo_index::min);
	uint32_t max = readRegister(fifo_index::max);
	uint32_t next_cmd = readRegister(fifo_index::next_cmd);

	bool reserveable = hasCapability(caps::fifo_reserve);

	assert(bytes < (max - min));
	assert(!(bytes % 4));
	assert(_reservedSize == 0);

	_reservedSize = bytes;

	_usingBounceBuf = false;

	while(1) {
		uint32_t stop = readRegister(fifo_index::stop);
		bool in_place = false;

		if (next_cmd >= stop) {
			if (next_cmd + bytes < max || 
				(next_cmd + bytes == max && stop > min)) in_place = true;

			else if ((max - next_cmd) + (stop - min) <= bytes) {
				_device->waitIrq(2); // TODO: add a definiton for the mask
			} else {
				_usingBounceBuf = true;
			}
		} else {
			if (next_cmd + bytes < stop) in_place = true;
			else _device->waitIrq(2); // TODO: add a definiton for the mask
		}

		if (in_place) {
			if (reserveable) {
				writeRegister(fifo_index::reserved, bytes);
				auto mem = static_cast<uint8_t *>(_fifoMapping.get());
				auto ptr = mem + next_cmd;

				return ptr;
			}
		}

		_usingBounceBuf = true;
		return _bounceBuf;
	}

	return nullptr;
}

void GfxDevice::DeviceFifo::commit(size_t bytes) {
	uint32_t min = readRegister(fifo_index::min);
	uint32_t max = readRegister(fifo_index::max);
	uint32_t next_cmd = readRegister(fifo_index::next_cmd);

	bool reserveable = hasCapability(caps::fifo_reserve);

	assert(_reservedSize > 0);

	_reservedSize = 0;

	if (_usingBounceBuf) {
		if (reserveable) {
			auto fifo = static_cast<uint8_t *>(_fifoMapping.get());
			
			auto chunk_size = std::min(bytes, (size_t)(max - next_cmd));
			writeRegister(fifo_index::reserved, bytes);
			memcpy(fifo + next_cmd, _bounceBuf, chunk_size);
			memcpy(fifo + min, &_bounceBuf[chunk_size], bytes - chunk_size);
		} else {
			auto buf = reinterpret_cast<uint32_t *>(_bounceBuf);
			auto fifo = reinterpret_cast<uint32_t *>(_fifoMapping.get());
			while (bytes) {
				fifo[next_cmd / 4] = *buf++;
				next_cmd += 4;
				if (next_cmd >= max) {
					next_cmd -= max - min;
				}
				writeRegister(fifo_index::next_cmd, next_cmd);
				bytes -= 4;
			}	
		}

	} else {
		next_cmd += bytes;
		if (next_cmd >= max)
			next_cmd -= max - min;

		writeRegister(fifo_index::next_cmd, next_cmd);
	}

	if (reserveable)
		writeRegister(fifo_index::reserved, 0);
}

void GfxDevice::DeviceFifo::commitAll() {
	commit(_reservedSize);
}

#define SVGA_BITMAP_SIZE(w, h)      ((((w) + 31) >> 5) * (h))
#define SVGA_PIXMAP_SIZE(w, h, bpp) (((((w) * (bpp)) + 31) >> 5) * (h))

void GfxDevice::DeviceFifo::defineCursor(int width, int height,
	GfxDevice::BufferObject *bo) {

	if (!_device->hasCapability(caps::cursor))
		return;

	// size in dwords
	size_t size = sizeof(commands::define_alpha_cursor) / 4 + 3 + SVGA_BITMAP_SIZE(width, height) + SVGA_PIXMAP_SIZE(width, height, 32);

	auto ptr = static_cast<uint32_t *>(reserve(size));

	ptr[0] = (uint32_t)command_index::define_cursor;
	auto cmd = reinterpret_cast<commands::define_cursor *>(ptr + 1);
	
	cmd->width = width;
	cmd->height = height;
	cmd->id = 0;
	cmd->hotspot_x = 1;
	cmd->hotspot_y = 1;
	cmd->bpp = 32;

	if (bo) {
		helix::Mapping bitmap{bo->getMemory().first, 0, (size_t)(width * height * 4)};

		auto pixels = static_cast<uint32_t *>(bitmap.get());
		auto mask = reinterpret_cast<uint8_t *>(cmd->pixel_data);

		memset(cmd->pixel_data, 0x00, (SVGA_BITMAP_SIZE(width, height) + SVGA_PIXMAP_SIZE(width, height, 32)) * 4);

		for (int i = 0; i < height; i++) {
			for (int j = 0; j < width; j++) {
				int idx = width * i + j;
				int bitmask = 1 << (7 - j % 8);
				int off = idx / 8;
				mask[off] |= !(pixels[idx] & 0xFF000000) ? bitmask : 0;
			}
		}

		memcpy(cmd->pixel_data + SVGA_BITMAP_SIZE(width, height) * 4, pixels, width * height * 4);
	}
	commitAll();
}

void GfxDevice::DeviceFifo::moveCursor(int x, int y) {

	if (!_device->hasCapability(caps::cursor))
		return;

	if (hasCapability(caps::fifo_cursor_bypass_3)) {
		writeRegister(fifo_index::cursor_x, x);
		writeRegister(fifo_index::cursor_y, y);
		writeRegister(fifo_index::cursor_count, readRegister(fifo_index::cursor_count) + 1);
		writeRegister(fifo_index::cursor_screen_id, 0xFFFFFFFF);
	} else {
		_device->writeRegister(register_index::cursor_x, x);
		_device->writeRegister(register_index::cursor_y, y);
	}
}

void GfxDevice::DeviceFifo::setCursorState(bool enabled) {
	if (!_device->hasCapability(caps::cursor))
		return;

	if (hasCapability(caps::fifo_cursor_bypass_3)) {
		writeRegister(fifo_index::cursor_on, enabled ? 1 : 0);
	} else {	
		_device->writeRegister(register_index::cursor_on, enabled ? 1 : 0);
	}
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
			if (assign.object == _device->_cursorPlane) {
				_cursorWidth = assign.intValue;
				_cursorUpdate = true;
			} else if (assign.object == _device->_primaryPlane) {
				_width = assign.intValue;
			}
		} else if (assign.property == _device->srcHProperty()) {
			assert(assign.property->validate(assign));
			if (assign.object == _device->_cursorPlane) {
				_cursorHeight = assign.intValue;
				_cursorUpdate = true;
			} else if (assign.object == _device->_primaryPlane) {
				_height = assign.intValue;
			}
		} else if (assign.property == _device->crtcXProperty()) {
			assert(assign.property->validate(assign));
			if (assign.object == _device->_cursorPlane) {
				_cursorX = assign.intValue;
				_cursorMove = true;
			}
		} else if (assign.property == _device->crtcYProperty()) {
			assert(assign.property->validate(assign));
			if (assign.object == _device->_cursorPlane) {
				_cursorY = assign.intValue;
				_cursorMove = true;
			}
		} else if (assign.property == _device->fbIdProperty()) {
			assert(assign.property->validate(assign));
			if (assign.objectValue) {
			auto fb = assign.objectValue->asFrameBuffer();
				if (assign.object == _device->_cursorPlane) {
					_cursorFb = static_cast<GfxDevice::FrameBuffer *>(fb);
					_cursorUpdate = true;
				} else if (assign.object == _device->_primaryPlane) {	
					_fb = static_cast<GfxDevice::FrameBuffer *>(fb);
				}
			}
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

	if (_cursorUpdate) {
		if (_cursorWidth != 0 && _cursorHeight != 0) {
			_device->_fifo.setCursorState(true);
			_device->_fifo.defineCursor(_cursorWidth, _cursorHeight, _cursorFb->getBufferObject());
			_device->_fifo.setCursorState(true);
		} else {
			_device->_fifo.setCursorState(false);
		}
	}

	if (_cursorMove) {
		_device->_fifo.moveCursor(_cursorX, _cursorY);
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

drm_core::Plane *GfxDevice::Crtc::Crtc::cursorPlane() {
	return _device->_cursorPlane.get();
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
	auto fifo_bar = COFIBER_AWAIT pci_device.accessBar(2);

	auto io_bar_info = info.barInfo[0];
	auto fb_bar_info = info.barInfo[1];
	auto fifo_bar_info = info.barInfo[2];

	auto gfx_device = std::make_shared<GfxDevice>(std::move(pci_device),
			helix::Mapping{fb_bar, 0, fb_bar_info.length},
			helix::Mapping{fifo_bar, 0, fifo_bar_info.length},
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

