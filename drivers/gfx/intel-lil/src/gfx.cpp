#include <span>

#include <arch/dma_structs.hpp>
#include <core/drm/debug.hpp>
#include <lil/imports.h>
#include <lil/intel.h>

#include "debug.hpp"
#include "gfx.hpp"

std::vector<std::shared_ptr<GfxDevice::Plane>> planes = {};
std::vector<std::shared_ptr<GfxDevice::Crtc>> crtcs = {};
std::vector<std::shared_ptr<GfxDevice::Connector>> connectors = {};
std::vector<std::shared_ptr<GfxDevice::Encoder>> encoders = {};

namespace {

static uintptr_t align_down(uintptr_t n, uintptr_t a) {
    return (n & ~(a - 1));
}

static uintptr_t align_up(uintptr_t n, uintptr_t a) {
    return align_down(n + a - 1, a);
}

}

void convertDrmModeInfo(drm_mode_modeinfo &r, LilModeInfo &lil) {
	r.clock = lil.clock;
	r.hdisplay = static_cast<__u16>(lil.hactive);
	r.hsync_start = static_cast<__u16>(lil.hsyncStart);
	r.hsync_end = static_cast<__u16>(lil.hsyncEnd);
	r.htotal = static_cast<__u16>(lil.htotal);
	r.vdisplay = static_cast<__u16>(lil.vactive);
	r.vsync_start = static_cast<__u16>(lil.vsyncStart);
	r.vsync_end = static_cast<__u16>(lil.vsyncEnd);
	r.vtotal = static_cast<__u16>(lil.vtotal);
}

void convertLilModeInfo(LilModeInfo &r, drm_mode_modeinfo &drm) {
	r.clock = drm.clock;
	r.hactive = drm.hdisplay;
	r.hsyncStart = drm.hsync_start;
	r.hsyncEnd = drm.hsync_end;
	r.htotal = drm.htotal;
	r.vactive = drm.vdisplay;
	r.vsyncStart = drm.vsync_start;
	r.vsyncEnd = drm.vsync_end;
	r.vtotal = drm.vtotal;
}

bool operator==(const LilModeInfo &lil, const drm_mode_modeinfo &drm) {
	return lil.clock == drm.clock &&
		lil.hactive == drm.hdisplay &&
		lil.hsyncStart == drm.hsync_start &&
		lil.hsyncEnd == drm.hsync_end &&
		lil.htotal == drm.htotal &&
		lil.vactive == drm.vdisplay &&
		lil.vsyncStart == drm.vsync_start &&
		lil.vsyncEnd == drm.vsync_end &&
		lil.vtotal == drm.vtotal;
}

GfxDevice::GfxDevice(protocols::hw::Device hw_device, uint16_t pch_devid)
	: _hwDevice{std::move(hw_device)}, _vramAllocator(26, 12), _pchDevId{pch_devid} {

};

async::result<std::unique_ptr<drm_core::Configuration>> GfxDevice::initialize() {
	std::vector<drm_core::Assignment> assignments;

	/* we want to get exclusive access to the GPU here already, as we will be modifying the GTT during the setup */
	co_await _hwDevice.claimDevice();

    lil_init_gpu(&_gpu, &_hwDevice, _pchDevId);

	arch::dma_buffer scratch{&_pool, 0x1000};
	uintptr_t phys = helix::ptrToPhysical(scratch.data());

	for(size_t i = 0; i < _gpu.gtt_size / 8; i++) {
		_gpu.vmem_map(&_gpu, phys, i << 12);
	}

    for(size_t i = 0; i < _gpu.num_connectors; i++) {
		LilConnector *lil_con = &_gpu.connectors[i];

		if(logLilVerbose) {
        	printf("gfx/intel-lil: Connector %zd, ID: %u\n", i, lil_con->id);

			if(!lil_con->is_connected) {
				printf(" cannot be used!\n");
				continue;
			}
		}

		LilCrtc *lil_crtc = lil_con->crtc;
		assert(lil_crtc);

		auto plane = std::make_shared<Plane>(this, Plane::PlaneType::PRIMARY, &lil_crtc->planes[0]);
		plane->setupWeakPtr(plane);
		plane->setupState(plane);
		plane->updateInFormatsBlob(plane);
		registerObject(plane.get());

		auto crtc = std::make_shared<Crtc>(this, lil_crtc, plane.get());
		crtc->setupWeakPtr(crtc);
		crtc->setupState(crtc);
		registerObject(crtc.get());

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

		plane->setupPossibleCrtcs({crtc.get()});

		setupCrtc(crtc.get());

		crtcs.push_back(crtc);
		planes.push_back(plane);

		setupMinDimensions(0, 0);
		setupMaxDimensions(16384, 16384);

		if(lil_con->is_connected(&_gpu, lil_con) && lil_con->get_connector_info) {
			auto encoder = std::make_shared<Encoder>(this);
			encoder->setupWeakPtr(encoder);
			registerObject(encoder.get());

			auto con = std::make_shared<Connector>(this, lil_con);
			con->setupWeakPtr(con);
			con->setupState(con);
			registerObject(con.get());

			assignments.push_back(drm_core::Assignment::withInt(con, dpmsProperty(), 3));
			assignments.push_back(drm_core::Assignment::withModeObj(con, crtcIdProperty(), crtc));

			encoder->setCurrentCrtc(crtc.get());
			encoder->setupPossibleCrtcs({crtc.get()});
			encoder->setupPossibleClones({encoder.get()});
			encoder->setupEncoderType(0);

			con->setupPossibleEncoders({encoder.get()});
			con->setCurrentEncoder(encoder.get());
			con->setCurrentStatus(1);
			con->setupSubpixel(0);

			setupEncoder(encoder.get());
			attachConnector(con.get());

			encoders.push_back(encoder);
			connectors.push_back(con);

			switch(lil_con->type) {
				case EDP:
					con->setConnectorType(DRM_MODE_CONNECTOR_eDP);
					break;
				case DISPLAYPORT:
					con->setConnectorType(DRM_MODE_CONNECTOR_DisplayPort);
					break;
				case HDMI:
					con->setConnectorType(DRM_MODE_CONNECTOR_HDMIA);
					break;
				case LVDS:
					con->setConnectorType(DRM_MODE_CONNECTOR_LVDS);
					break;
			}

			std::vector<drm_mode_modeinfo> supported_modes;

			LilConnectorInfo info = {};
			info = lil_con->get_connector_info(&_gpu, lil_con);

			for(size_t j = 0; j < info.num_modes; j++) {
				if(!info.modes[j].hactive || !info.modes[j].vactive || !info.modes[j].clock) {
					continue;
				}

				if(logLilVerbose)
					printf("\tmode %dx%d clock %u\n", info.modes[j].hactive, info.modes[j].vactive, info.modes[j].clock);

				auto name = std::to_string(info.modes[j].hactive) + "x" + std::to_string(info.modes[j].vactive);

				drm_mode_modeinfo drm_mode{
					.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC,
					.type = DRM_MODE_TYPE_DRIVER,
				};

				strncpy(drm_mode.name, name.c_str(), sizeof(drm_mode.name));

				if(!j)
					drm_mode.type |= DRM_MODE_TYPE_PREFERRED;

				convertDrmModeInfo(drm_mode, info.modes[j]);
				supported_modes.push_back(drm_mode);
			}

			lil_free(info.modes);

			con->setModeList(supported_modes);
		}

		lil_crtc->shutdown(&_gpu, lil_crtc);
	}

	auto config = createConfiguration();
	auto state = atomicState();
	assert(config->capture(assignments, state));
	config->commit(std::move(state));

	co_return std::move(config);
}

std::unique_ptr<drm_core::Configuration> GfxDevice::createConfiguration() {
	return std::make_unique<Configuration>(this);
}

std::pair<std::shared_ptr<drm_core::BufferObject>, uint32_t> GfxDevice::createDumb(uint32_t width,
		uint32_t height, uint32_t bpp) {
	size_t pitch = ((width * (bpp / 8) + 63) & ~0x3F);
	size_t size = height * pitch;

	auto offset = _vramAllocator.allocate(size);
	auto buffer = std::make_shared<GfxDevice::BufferObject>(this, offset, width, height, size);

	auto mapping = installMapping(buffer.get());
	buffer->setupMapping(mapping);
	return std::make_pair(std::move(buffer), pitch);
}

std::shared_ptr<drm_core::FrameBuffer> GfxDevice::createFrameBuffer(std::shared_ptr<drm_core::BufferObject> base_bo,
		uint32_t width, uint32_t height, uint32_t format, uint32_t pitch) {
	auto bo = std::static_pointer_cast<GfxDevice::BufferObject>(base_bo);
	auto f = drm_core::getFormatInfo(format);
	assert(f);
	auto bpp = drm_core::getFormatBpp(*f, 0);

	auto pixel_pitch = pitch / (bpp / 8);
	assert(pixel_pitch >= width);

	assert(bo->getSize() >= pitch * height);

	auto fb = std::make_shared<FrameBuffer>(this, bo, pixel_pitch);
	fb->setupWeakPtr(fb);
	registerObject(fb.get());

	fb->setFormat(format);

	return fb;
}

//returns major, minor, patchlvl
std::tuple<int, int, int> GfxDevice::driverVersion() {
	return {1, 0, 0};
}

//returns name, desc, date
std::tuple<std::string, std::string, std::string> GfxDevice::driverInfo() {
	return { "intel-lil", "Intel GPU driver based on lil", "0"};
}

bool GfxDevice::Configuration::capture(std::vector<drm_core::Assignment> assignment, std::unique_ptr<drm_core::AtomicState> &state) {
	if(logDrmRequests)
		std::cout << "gfx/intel-lil: Configuration capture" << std::endl;

	for(auto &assign: assignment) {
		assert(assign.property->validate(assign));
		assign.property->writeToState(assign, state);

		if(!logDrmRequests)
			continue;

		auto mode_obj = assign.object;

		switch(mode_obj->type()) {
			case drm_core::ObjectType::crtc:
				std::cout << "\tCRTC (ID " << mode_obj->id() << ")" << std::endl;
				break;
			case drm_core::ObjectType::connector:
				std::cout << "\tConnector (ID " << mode_obj->id() << ")" << std::endl;
				break;
			case drm_core::ObjectType::encoder:
				std::cout << "\tEncoder (ID " << mode_obj->id() << ")" << std::endl;
				break;
			case drm_core::ObjectType::frameBuffer:
				std::cout << "\tFB (ID " << mode_obj->id() << ")" << std::endl;
				break;
			case drm_core::ObjectType::plane:
				std::cout << "\tPlane (ID " << mode_obj->id() << ")" << std::endl;
				break;
		}

		auto prop_type = assign.property->propertyType();

		if(std::holds_alternative<drm_core::IntPropertyType>(prop_type)) {
			std::cout << "\t\t" << assign.property->name() << " = " << assign.intValue << " (int)" << std::endl;
		} else if(std::holds_alternative<drm_core::EnumPropertyType>(prop_type)) {
			std::cout << "\t\t" << assign.property->name() << " = " << assign.intValue << " " << assign.property->enumInfo().at(assign.intValue) << " (enum)" << std::endl;
		} else if(std::holds_alternative<drm_core::BlobPropertyType>(prop_type)) {
			std::cout << "\t\t" << assign.property->name() << " = " << (assign.blobValue ? std::to_string(assign.blobValue->id()) : "<none>") << " (blob)" << std::endl;
		} else if(std::holds_alternative<drm_core::ObjectPropertyType>(prop_type)) {
			std::cout << "\t\t" << assign.property->name() << " = " << (assign.objectValue ? std::to_string(assign.objectValue->id()) : "<none>") << " (modeobject)" << std::endl;
		}
	}

	return true;
}

void GfxDevice::Configuration::dispose() {

}

void GfxDevice::Configuration::commit(std::unique_ptr<drm_core::AtomicState> state) {
	_doCommit(std::move(state));
}

async::detached GfxDevice::Configuration::_doCommit(std::unique_ptr<drm_core::AtomicState> state) {
	for(auto [id, plane_state] : state->plane_states()) {
		auto plane = std::static_pointer_cast<GfxDevice::Plane>(plane_state->plane);
		auto crtc = std::static_pointer_cast<GfxDevice::Crtc>(plane_state->crtc);

		if(!crtc)
			continue;

		auto crtc_state = state->crtc(crtc->id());
		LilCrtc *lil_crtc = crtc->_lil;
		LilConnector *con = lil_crtc->connector;
		LilPlane *lil_plane = plane->_lil;
		auto fb = static_cast<GfxDevice::FrameBuffer *>(plane->getFrameBuffer());

		auto old_mode = crtc->drmState()->mode;
		auto new_mode = crtc_state->mode;
		auto drm_mode = new_mode ? reinterpret_cast<const struct drm_mode_modeinfo *>(new_mode->data()) : nullptr;
		// TODO: fail validation if this happens
		assert(bool(drm_mode) == crtc_state->active);

		bool mode_changed = (!old_mode ^ !new_mode) || (old_mode && new_mode && old_mode->id() != new_mode->id());
		auto switch_mode = mode_changed || crtc->drmState()->active != crtc_state->active;

		if(switch_mode) {
			struct Mode {
				size_t width, height, pitch;
				uint8_t bpp;
			};

			LilConnectorInfo info = {};
			int selected_mode = -1;

			if(!con->is_connected || !con->is_connected(&_device->_gpu, con)) {
				if(logLilVerbose)
					printf("gfx/intel-lil: Connector ID %u is disconnected!\n", con->id);
				continue;
			}

			if(!con->get_connector_info) {
				if(logLilVerbose)
					printf("gfx/intel-lil: Connector ID %u can't read connector info!\n", con->id);
				continue;
			}

			if(drm_mode) {
				info = con->get_connector_info(&_device->_gpu, con);

				for(size_t j = 0; j < 4; j++) {
					if(*drm_mode == info.modes[j]) {
						selected_mode = j;
						break;
					}
				}
			}

			if(selected_mode != -1 && crtc_state->active) {
				con->crtc->current_mode = info.modes[selected_mode];
				lil_crtc->shutdown(&_device->_gpu, lil_crtc);

				lil_plane->enabled = true;
				lil_plane->pixel_format = fb->format();

				lil_crtc->commit_modeset(&_device->_gpu, lil_crtc);

				if(logLilVerbose)
					printf("gfx/intel-lil: mode %ux%u has been set on CRTC %u\n", info.modes[selected_mode].hactive, info.modes[selected_mode].vactive, crtc->id());
			} else if(!crtc_state->active) {
				lil_crtc->shutdown(&_device->_gpu, lil_crtc);
			} else {
				lil_panic("no appropriate mode found");
			}
		}

		if(fb && fb->getBufferObject()) {
			auto pitch = align_up(drm_mode->hdisplay * 4, 64);

			if(!lil_plane->update_surface(&_device->_gpu, lil_plane, fb->getBufferObject()->getAddress(), pitch)) {
				lil_panic("primary plane update failed");
			}
		}
	}

	for(auto [_, plane_state] : state->plane_states()) {
		plane_state->plane->setDrmState(plane_state);
	}

	for(auto [_, crtc_state] : state->crtc_states()) {
		crtc_state->crtc().lock()->setDrmState(crtc_state);
	}

	for(auto [_, connector_state] : state->connector_states()) {
		connector_state->connector->setDrmState(connector_state);
	}

	complete();

	co_return;
}

GfxDevice::Crtc::Crtc(GfxDevice *device, LilCrtc *lil, Plane *primary)
		: drm_core::Crtc { device, device->allocator.allocate() }, _lil{lil}, _primaryPlane{primary} {
	_device = device;
}

drm_core::Plane *GfxDevice::Crtc::primaryPlane() {
	return _primaryPlane;
}

GfxDevice::Connector::Connector(GfxDevice *device, LilConnector *lil)
		: drm_core::Connector { device, device->allocator.allocate() }, _lil{lil} {

}

GfxDevice::Plane::Plane(GfxDevice *device, PlaneType type, LilPlane *lil)
		: drm_core::Plane { device, device->allocator.allocate(), type }, _lil{lil} {
	size_t formats = 0;
	uint32_t *base = _lil->get_formats(&device->_gpu, &formats);

	clearFormats();

	for(auto s : std::span<uint32_t>{base, formats}) {
		addFormat(s);
	}
}

GfxDevice::Encoder::Encoder(GfxDevice *device)
		: drm_core::Encoder { device, device->allocator.allocate() } {
}

GfxDevice::FrameBuffer::FrameBuffer(GfxDevice *device, std::shared_ptr<GfxDevice::BufferObject> bo, uint32_t pixel_pitch)
		: drm_core::FrameBuffer { device, device->allocator.allocate() } {
	_bo = bo;
	_pixelPitch = pixel_pitch;
}

void GfxDevice::FrameBuffer::notifyDirty() {

}

uint32_t GfxDevice::FrameBuffer::getWidth() {
	return _bo->getWidth();
}

uint32_t GfxDevice::FrameBuffer::getHeight() {
	return _bo->getHeight();
}

GfxDevice::BufferObject *GfxDevice::FrameBuffer::getBufferObject() {
	return _bo.get();
}

GfxDevice::BufferObject::BufferObject(GfxDevice *device, GpuAddr gpu_addr, uint32_t width, uint32_t height, size_t size)
: drm_core::BufferObject{width, height}, _device{device}, _gpuAddr{gpu_addr}, _size{size} {
	_size = (_size + (4096 - 1)) & ~(4096 - 1);

	HelHandle handle;
	HEL_CHECK(helAllocateMemory(size, 0, nullptr, &handle));
	HEL_CHECK(helMapMemory(handle, kHelNullHandle, nullptr, 0, _size, kHelMapProtRead, &_ptr));
	_memoryView = helix::UniqueDescriptor{handle};

	for(size_t page = 0; page < _size; page += 0x1000) {
		uintptr_t phys = helix::addressToPhysical(uintptr_t(_ptr) + page);

		_device->_gpu.vmem_map(&_device->_gpu, phys, _gpuAddr + page);
	}
};

GpuAddr GfxDevice::BufferObject::getAddress() {
	return _gpuAddr;
}

std::shared_ptr<drm_core::BufferObject> GfxDevice::BufferObject::sharedBufferObject() {
	return this->shared_from_this();
}

size_t GfxDevice::BufferObject::getSize() {
	return _size;
}

std::pair<helix::BorrowedDescriptor, uint64_t> GfxDevice::BufferObject::getMemory() {
	return std::make_pair(helix::BorrowedDescriptor{_memoryView}, 0);
}
