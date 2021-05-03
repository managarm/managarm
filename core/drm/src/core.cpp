
#include <assert.h>
#include <stdio.h>
#include <deque>
#include <experimental/optional>
#include <optional>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>
#include <sys/epoll.h>

#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/defs.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <libdrm/drm_fourcc.h>

#include "fs.bragi.hpp"
#include "core/drm/core.hpp"

// ----------------------------------------------------------------
// Device
// ----------------------------------------------------------------

drm_core::Device::Device() {
	struct SrcWProperty : drm_core::Property {
		SrcWProperty()
		: drm_core::Property{drm_core::IntPropertyType{}} { }

		bool validate(const Assignment& assignment) override {
			return true;
		};
	};
	_srcWProperty = std::make_shared<SrcWProperty>();
	
	struct SrcHProperty : drm_core::Property {
		SrcHProperty()
		: drm_core::Property{drm_core::IntPropertyType{}} { }

		bool validate(const Assignment& assignment) override {
			return true;
		};
	};
	_srcHProperty = std::make_shared<SrcHProperty>();

	struct FbIdProperty : drm_core::Property {
		FbIdProperty()
		: drm_core::Property{drm_core::ObjectPropertyType{}} { }
		
		bool validate(const Assignment& assignment) override {
			if(!assignment.objectValue)
				return true;

			if(assignment.objectValue->asFrameBuffer())
				return true;

			return false;
		};
	};
	_fbIdProperty = std::make_shared<FbIdProperty>();

	struct ModeIdProperty : drm_core::Property {
		ModeIdProperty()
		: drm_core::Property{drm_core::BlobPropertyType{}} { }

		bool validate(const Assignment& assignment) override {
			if(!assignment.blobValue)
				return true;

			if(assignment.blobValue->size() != sizeof(drm_mode_modeinfo))
				return false;
			
			drm_mode_modeinfo mode_info;
			memcpy(&mode_info, assignment.blobValue->data(), sizeof(drm_mode_modeinfo));
			if(mode_info.hdisplay > mode_info.hsync_start)
				return false;
			if(mode_info.hsync_start > mode_info.hsync_end)
				return false;
			if(mode_info.hsync_end > mode_info.htotal)
				return false;

			if(mode_info.vdisplay > mode_info.vsync_start)
				return false;
			if(mode_info.vsync_start > mode_info.vsync_end)
				return false;
			if(mode_info.vsync_end > mode_info.vtotal)
				return false;

			return true;
		};
	};
	_modeIdProperty = std::make_shared<ModeIdProperty>();

	struct CrtcXProperty : drm_core::Property {
		CrtcXProperty()
		: drm_core::Property{drm_core::IntPropertyType{}} { }

		bool validate(const Assignment& assignment) override {
			return true;
		};
	};
	_crtcXProperty = std::make_shared<CrtcXProperty>();
	
	struct CrtcYProperty : drm_core::Property {
		CrtcYProperty()
		: drm_core::Property{drm_core::IntPropertyType{}} { }

		bool validate(const Assignment& assignment) override {
			return true;
		};
	};
	_crtcYProperty = std::make_shared<CrtcYProperty>();


}

void drm_core::Device::setupCrtc(drm_core::Crtc *crtc) {
	crtc->index = _crtcs.size();
	_crtcs.push_back(crtc);
}

void drm_core::Device::setupEncoder(drm_core::Encoder *encoder) {
	encoder->index = _encoders.size();
	_encoders.push_back(encoder);
}

void drm_core::Device::attachConnector(drm_core::Connector *connector) {
	_connectors.push_back(connector);
}

const std::vector<drm_core::Crtc *> &drm_core::Device::getCrtcs() {
	return _crtcs;
}

const std::vector<drm_core::Encoder *> &drm_core::Device::getEncoders() {
	return _encoders;
}

const std::vector<drm_core::Connector *> &drm_core::Device::getConnectors() {
	return _connectors;
}

void drm_core::Device::registerObject(drm_core::ModeObject *object) {
	_objects.insert({object->id(), object});
}

std::shared_ptr<drm_core::ModeObject> drm_core::Device::findObject(uint32_t id) {
	auto it = _objects.find(id);
	if(it == _objects.end())
		return nullptr;
	return it->second->sharedModeObject();
}

uint64_t drm_core::Device::installMapping(drm_core::BufferObject *bo) {
	assert(bo->getSize() < (UINT64_C(1) << 32));
	return static_cast<uint64_t>(_memorySlotAllocator.allocate()) << 32;
}
	
void drm_core::Device::setupMinDimensions(uint32_t width, uint32_t height) {
	_minWidth = width;
	_minHeight = height;
}

void drm_core::Device::setupMaxDimensions(uint32_t width, uint32_t height) {
	_maxWidth = width;
	_maxHeight = height;
}

uint32_t drm_core::Device::getMinWidth() {
	return _minWidth;
}

uint32_t drm_core::Device::getMaxWidth() {
	return _maxWidth;
}

uint32_t drm_core::Device::getMinHeight() {
	return _minHeight;
}

uint32_t drm_core::Device::getMaxHeight() {
	return _maxHeight;
}
	
drm_core::Property *drm_core::Device::srcWProperty() {
	return _srcWProperty.get();
}

drm_core::Property *drm_core::Device::srcHProperty() {
	return _srcHProperty.get();
}

drm_core::Property *drm_core::Device::fbIdProperty() {
	return _fbIdProperty.get();
}

drm_core::Property *drm_core::Device::modeIdProperty() {
	return _modeIdProperty.get();
}

drm_core::Property *drm_core::Device::crtcXProperty() {
	return _crtcXProperty.get();
}

drm_core::Property *drm_core::Device::crtcYProperty() {
	return _crtcYProperty.get();
}
	
// ----------------------------------------------------------------
// Property
// ----------------------------------------------------------------

bool drm_core::Property::validate(const Assignment&) {
	return true;
}

drm_core::PropertyType drm_core::Property::propertyType() {
	return _propertyType;
}

// ----------------------------------------------------------------
// BufferObject
// ----------------------------------------------------------------

void drm_core::BufferObject::setupMapping(uint64_t mapping) {
	_mapping = mapping;
}

uint64_t drm_core::BufferObject::getMapping() {
	return _mapping;
}

// ----------------------------------------------------------------
// Encoder
// ----------------------------------------------------------------

drm_core::Encoder::Encoder(uint32_t id)
	:drm_core::ModeObject { ObjectType::encoder, id } {
	index = -1;
	_currentCrtc = nullptr;
}

drm_core::Crtc *drm_core::Encoder::currentCrtc() {
	return _currentCrtc;
}

void drm_core::Encoder::setCurrentCrtc(drm_core::Crtc *crtc) {
	_currentCrtc = crtc;
}
	
void drm_core::Encoder::setupEncoderType(uint32_t type) {
	_encoderType = type;
}
	
uint32_t drm_core::Encoder::getEncoderType() {
	return _encoderType;
}
	
void drm_core::Encoder::setupPossibleCrtcs(std::vector<drm_core::Crtc *> crtcs) {
	_possibleCrtcs = crtcs;
}
	
const std::vector<drm_core::Crtc *> &drm_core::Encoder::getPossibleCrtcs() {
	return _possibleCrtcs;
}
	
void drm_core::Encoder::setupPossibleClones(std::vector<drm_core::Encoder *> clones) {
	_possibleClones = clones;
}
	
const std::vector<drm_core::Encoder *> &drm_core::Encoder::getPossibleClones() {
	return _possibleClones;
}

// ----------------------------------------------------------------
// ModeObject
// ----------------------------------------------------------------

uint32_t drm_core::ModeObject::id() {
	return _id;
}

drm_core::Encoder *drm_core::ModeObject::asEncoder() {
	if(_type != ObjectType::encoder)
		return nullptr;
	return static_cast<Encoder *>(this);
}

drm_core::Connector *drm_core::ModeObject::asConnector() {
	if(_type != ObjectType::connector)
		return nullptr;
	return static_cast<Connector *>(this);
}

drm_core::Crtc *drm_core::ModeObject::asCrtc() {
	if(_type != ObjectType::crtc)
		return nullptr;
	return static_cast<Crtc *>(this);
}

drm_core::FrameBuffer *drm_core::ModeObject::asFrameBuffer() {
	if(_type != ObjectType::frameBuffer)
		return nullptr;
	return static_cast<FrameBuffer *>(this);
}

drm_core::Plane *drm_core::ModeObject::asPlane() {
	if(_type != ObjectType::plane)
		return nullptr;
	return static_cast<Plane *>(this);
}

void drm_core::ModeObject::setupWeakPtr(std::weak_ptr<ModeObject> self) {
	_self = self;
}

std::shared_ptr<drm_core::ModeObject> drm_core::ModeObject::sharedModeObject() {
	return _self.lock();
}

// ----------------------------------------------------------------
// Crtc
// ----------------------------------------------------------------

drm_core::Crtc::Crtc(uint32_t id)
	:drm_core::ModeObject { ObjectType::crtc, id } {
	index = -1;
}

std::shared_ptr<drm_core::Blob> drm_core::Crtc::currentMode() {
	return _curMode;
}
	
void drm_core::Crtc::setCurrentMode(std::shared_ptr<drm_core::Blob> mode) {
	_curMode = mode;
}

drm_core::Plane *drm_core::Crtc::cursorPlane() {
	return nullptr;
}

// ----------------------------------------------------------------
// FrameBuffer
// ----------------------------------------------------------------

drm_core::FrameBuffer::FrameBuffer(uint32_t id)
	:drm_core::ModeObject { ObjectType::frameBuffer, id } {
}

// ----------------------------------------------------------------
// Plane
// ----------------------------------------------------------------

drm_core::Plane::Plane(uint32_t id)
	:drm_core::ModeObject { ObjectType::plane, id } {
}

// ----------------------------------------------------------------
// Connector
// ----------------------------------------------------------------

drm_core::Connector::Connector(uint32_t id)
	:drm_core::ModeObject { ObjectType::connector, id } {
	_currentEncoder = nullptr;
	_connectorType = 0;
}

const std::vector<drm_mode_modeinfo> &drm_core::Connector::modeList() {
	return _modeList;
}

void drm_core::Connector::setModeList(std::vector<drm_mode_modeinfo> mode_list) {
	_modeList = mode_list;
}
	
void drm_core::Connector::setCurrentStatus(uint32_t status) {
	_currentStatus = status;
}
	
void drm_core::Connector::setCurrentEncoder(drm_core::Encoder *encoder) {
	_currentEncoder = encoder;
}
	
drm_core::Encoder *drm_core::Connector::currentEncoder() {
	return _currentEncoder;
}

uint32_t drm_core::Connector::getCurrentStatus() {
	return _currentStatus;
}
	
void drm_core::Connector::setupPossibleEncoders(std::vector<drm_core::Encoder *> encoders) {
	_possibleEncoders = encoders;
}

const std::vector<drm_core::Encoder *> &drm_core::Connector::getPossibleEncoders() {
	return _possibleEncoders;
}
	
void drm_core::Connector::setupPhysicalDimensions(uint32_t width, uint32_t height) {
	_physicalWidth = width;
	_physicalHeight = height;
}

uint32_t drm_core::Connector::getPhysicalWidth() {
	return _physicalWidth;
}

uint32_t drm_core::Connector::getPhysicalHeight() {
	return _physicalHeight;
}

void drm_core::Connector::setupSubpixel(uint32_t subpixel) {
	_subpixel = subpixel;
}

uint32_t drm_core::Connector::getSubpixel() {
	return _subpixel;
}

uint32_t drm_core::Connector::connectorType() {
	return _connectorType;
}

// ----------------------------------------------------------------
// Blob
// ----------------------------------------------------------------

size_t drm_core::Blob::size() {
	return _data.size();
}
	
const void *drm_core::Blob::data() {
	return _data.data();
}

// ----------------------------------------------------------------
// File
// ----------------------------------------------------------------

drm_core::File::File(std::shared_ptr<Device> device)
: _device(device), _eventSequence{1} {
	HelHandle handle;
	HEL_CHECK(helCreateIndirectMemory(1024, &handle));
	_memory = helix::UniqueDescriptor{handle};

	_statusPage.update(_eventSequence, 0);
};

void drm_core::File::setBlocking(bool blocking) {
	_isBlocking = blocking;
}

void drm_core::File::attachFrameBuffer(std::shared_ptr<drm_core::FrameBuffer> frame_buffer) {
	_frameBuffers.push_back(frame_buffer);
}
	
void drm_core::File::detachFrameBuffer(drm_core::FrameBuffer *frame_buffer) {
	auto it = std::find_if(_frameBuffers.begin(), _frameBuffers.end(),
			([&](std::shared_ptr<drm_core::FrameBuffer> fb) {
				return fb.get() == frame_buffer;
			}));
	assert(it != _frameBuffers.end());
	_frameBuffers.erase(it);
}
	
const std::vector<std::shared_ptr<drm_core::FrameBuffer>> &drm_core::File::getFrameBuffers() {
	return _frameBuffers;
}
	
uint32_t drm_core::File::createHandle(std::shared_ptr<BufferObject> bo) {
	auto handle = _allocator.allocate();
	_buffers.insert({handle, bo});

	auto [boMemory, boOffset] = bo->getMemory();
	HEL_CHECK(helAlterMemoryIndirection(_memory.getHandle(),
			bo->getMapping() >> 32, boMemory.getHandle(),
			boOffset, bo->getSize()));

	return handle;
}
	
drm_core::BufferObject *drm_core::File::resolveHandle(uint32_t handle) {
	auto it = _buffers.find(handle);
	if(it == _buffers.end())
		return nullptr;
	return it->second.get();
};

void drm_core::File::postEvent(drm_core::Event event) {
	HEL_CHECK(helGetClock(&event.timestamp));

	if(_pendingEvents.empty()) {
		++_eventSequence;
		_statusPage.update(_eventSequence, EPOLLIN);
	}
	_pendingEvents.push_back(event);
	_eventBell.raise();
}

async::result<protocols::fs::ReadResult>
drm_core::File::read(void *object, const char *,
		void *buffer, size_t length) {
	auto self = static_cast<drm_core::File *>(object);

	if(!self->_isBlocking && self->_pendingEvents.empty())
		co_return protocols::fs::Error::wouldBlock;
	while(self->_pendingEvents.empty())
		co_await self->_eventBell.async_wait();

	auto ev = &self->_pendingEvents.front();

	// TODO: Support sequence number and CRTC id.
	drm_event_vblank out;
	memset(&out, 0, sizeof(drm_event_vblank));
	out.base.type = DRM_EVENT_FLIP_COMPLETE;
	out.base.length = sizeof(drm_event_vblank);
	out.user_data = ev->cookie;
	out.tv_sec = ev->timestamp / 1000000000;
	out.tv_usec = (ev->timestamp % 1000000000) / 1000;

	assert(length >= sizeof(drm_event_vblank));
	memcpy(buffer, &out, sizeof(drm_event_vblank));
	
	self->_pendingEvents.pop_front();
	if(self->_pendingEvents.empty())
		self->_statusPage.update(self->_eventSequence, 0);

	co_return sizeof(drm_event_vblank);
}

async::result<helix::BorrowedDescriptor>
drm_core::File::accessMemory(void *object) {
	auto self = static_cast<drm_core::File *>(object);
	co_return self->_memory;
}

async::result<void>
drm_core::File::ioctl(void *object, managarm::fs::CntRequest req,
		helix::UniqueLane conversation) {
	auto self = static_cast<drm_core::File *>(object);
	if(req.command() == DRM_IOCTL_VERSION) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;
	
		auto version = self->_device->driverVersion();
		auto info = self->_device->driverInfo();
		
		resp.set_drm_version_major(std::get<0>(version));
		resp.set_drm_version_minor(std::get<1>(version));
		resp.set_drm_version_patchlevel(std::get<2>(version));

		resp.set_drm_driver_name(std::get<0>(info));
		resp.set_drm_driver_desc(std::get<1>(info));
		resp.set_drm_driver_date(std::get<2>(info));

		resp.set_error(managarm::fs::Errors::SUCCESS);
		
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_GET_CAP) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;
		
		if(req.drm_capability() == DRM_CAP_TIMESTAMP_MONOTONIC) {
			resp.set_drm_value(1);
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}else if(req.drm_capability() == DRM_CAP_DUMB_BUFFER) {
			resp.set_drm_value(1);
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}else{
			std::cout << "core/drm: Unknown capability " << req.drm_capability() << std::endl;
			resp.set_drm_value(0);
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}
		
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETRESOURCES) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto &crtcs = self->_device->getCrtcs();
		for(int i = 0; i < crtcs.size(); i++) {
			resp.add_drm_crtc_ids(crtcs[i]->id());
		}
			
		auto &encoders = self->_device->getEncoders();
		for(int i = 0; i < encoders.size(); i++) {
			resp.add_drm_encoder_ids(encoders[i]->id());
		}
	
		auto &connectors = self->_device->getConnectors();
		for(int i = 0; i < connectors.size(); i++) {
			resp.add_drm_connector_ids(connectors[i]->id());
		}
		
		auto &fbs = self->getFrameBuffers();
		for(int i = 0; i < fbs.size(); i++) {
			resp.add_drm_fb_ids(fbs[i]->id());
		}
	
		resp.set_drm_min_width(self->_device->getMinWidth());
		resp.set_drm_max_width(self->_device->getMaxWidth());
		resp.set_drm_min_height(self->_device->getMinHeight());
		resp.set_drm_max_height(self->_device->getMaxHeight());
		resp.set_error(managarm::fs::Errors::SUCCESS);
	
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETCONNECTOR) {
		helix::SendBuffer send_resp;
		helix::SendBuffer send_list;
		managarm::fs::SvrResponse resp;
	
		auto obj = self->_device->findObject(req.drm_connector_id());
		assert(obj);
		auto conn = obj->asConnector();
		assert(conn);
		
		auto psbl_enc = conn->getPossibleEncoders();
		for(int i = 0; i < psbl_enc.size(); i++) { 
			resp.add_drm_encoders(psbl_enc[i]->id());
		}

		resp.set_drm_encoder_id(conn->currentEncoder()->id());
		resp.set_drm_connector_type(conn->connectorType());
		resp.set_drm_connector_type_id(0);
		resp.set_drm_connection(conn->getCurrentStatus()); // DRM_MODE_CONNECTED
		resp.set_drm_mm_width(conn->getPhysicalWidth());
		resp.set_drm_mm_height(conn->getPhysicalHeight());
		resp.set_drm_subpixel(conn->getSubpixel());
		resp.set_drm_num_modes(conn->modeList().size());
		resp.set_error(managarm::fs::Errors::SUCCESS);
	
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
			helix::action(&send_list, conn->modeList().data(),
					std::min(static_cast<size_t>(req.drm_max_modes()), conn->modeList().size())
							* sizeof(drm_mode_modeinfo)));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_list.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETENCODER) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		resp.set_drm_encoder_type(0);

		auto obj = self->_device->findObject(req.drm_encoder_id());
		assert(obj);
		auto enc = obj->asEncoder();
		assert(enc);
		resp.set_drm_crtc_id(enc->currentCrtc()->id());
		
		uint32_t crtc_mask = 0;
		for(auto crtc : enc->getPossibleCrtcs()) {
			crtc_mask |= 1 << crtc->index;
		}
		resp.set_drm_possible_crtcs(crtc_mask);
		
		uint32_t clone_mask = 0;
		for(auto clone : enc->getPossibleClones()) {
			clone_mask |= 1 << clone->index;
		}
		resp.set_drm_possible_clones(clone_mask);

		resp.set_error(managarm::fs::Errors::SUCCESS);
	
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_CREATE_DUMB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto pair = self->_device->createDumb(req.drm_width(), req.drm_height(), req.drm_bpp());
		auto handle = self->createHandle(pair.first);
		resp.set_drm_handle(handle);

		resp.set_drm_pitch(pair.second);
		resp.set_drm_size(pair.first->getSize());
		resp.set_error(managarm::fs::Errors::SUCCESS);
	
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_ADDFB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto bo = self->resolveHandle(req.drm_handle());
		assert(bo);
		auto buffer = bo->sharedBufferObject();
		
		auto fourcc = convertLegacyFormat(req.drm_bpp(), req.drm_depth());
		auto fb = self->_device->createFrameBuffer(buffer, req.drm_width(), req.drm_height(),
				fourcc, req.drm_pitch());
		self->attachFrameBuffer(fb);
		resp.set_drm_fb_id(fb->id());
		resp.set_error(managarm::fs::Errors::SUCCESS);
	
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_RMFB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;
		
		auto obj = self->_device->findObject(req.drm_fb_id());
		assert(obj);
		auto fb = obj->asFrameBuffer();
		assert(fb);
		self->detachFrameBuffer(fb);
		resp.set_error(managarm::fs::Errors::SUCCESS);
		
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_MAP_DUMB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto bo = self->resolveHandle(req.drm_handle());
		assert(bo);
		auto buffer = bo->sharedBufferObject();
	
		resp.set_drm_offset(buffer->getMapping());
		resp.set_error(managarm::fs::Errors::SUCCESS);
	
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETCRTC) {
		helix::SendBuffer send_resp;
		helix::SendBuffer send_mode;
		managarm::fs::SvrResponse resp;

		auto obj = self->_device->findObject(req.drm_crtc_id());
		assert(obj);
		auto crtc = obj->asCrtc();
		assert(crtc);

		drm_mode_modeinfo mode_info;
		if(crtc->currentMode()) {
			/* TODO: Set x, y, fb_id, gamma_size */
			memcpy(&mode_info, crtc->currentMode()->data(), sizeof(drm_mode_modeinfo));
			resp.set_drm_mode_valid(1);
		}else{
			memset(&mode_info, 0, sizeof(drm_mode_modeinfo));
			resp.set_drm_mode_valid(0);
		}
			
		resp.set_error(managarm::fs::Errors::SUCCESS);
	
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
			helix::action(&send_mode, &mode_info, sizeof(drm_mode_modeinfo)));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_mode.error());
	}else if(req.command() == DRM_IOCTL_MODE_SETCRTC) {
		std::vector<char> mode_buffer;
		mode_buffer.resize(sizeof(drm_mode_modeinfo));

		helix::RecvBuffer recv_buffer;
		auto &&buff = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&recv_buffer, mode_buffer.data(), sizeof(drm_mode_modeinfo)));
		co_await buff.async_wait();
		HEL_CHECK(recv_buffer.error());
		
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;
	
		auto obj = self->_device->findObject(req.drm_crtc_id());
		assert(obj);
		auto crtc = obj->asCrtc();
		assert(crtc);
	
		std::vector<drm_core::Assignment> assignments;
		if(req.drm_mode_valid()) {
			auto mode_blob = std::make_shared<Blob>(std::move(mode_buffer));
			assignments.push_back(Assignment{ 
				crtc->sharedModeObject(),
				self->_device->modeIdProperty(),
				0,
				nullptr,
				mode_blob
			});
			
			auto fb = self->_device->findObject(req.drm_fb_id());
			assert(fb);
			assignments.push_back(Assignment{ 
				crtc->primaryPlane()->sharedModeObject(),
				self->_device->fbIdProperty(),
				0,
				fb,
				nullptr
			});
		}else{
			assignments.push_back(Assignment{ 
				crtc->sharedModeObject(),
				self->_device->modeIdProperty(),
				0,
				nullptr,
				nullptr
			});
		}

		auto config = self->_device->createConfiguration();
		auto valid = config->capture(assignments);
		assert(valid);
		config->commit();

		co_await config->waitForCompletion();

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_PAGE_FLIP) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;
	
		auto obj = self->_device->findObject(req.drm_crtc_id());
		assert(obj);
		auto crtc = obj->asCrtc();
		assert(crtc);
	
		std::vector<drm_core::Assignment> assignments;

		auto fb = self->_device->findObject(req.drm_fb_id());
		assert(fb);
		assignments.push_back(Assignment{ 
			crtc->primaryPlane()->sharedModeObject(),
			self->_device->fbIdProperty(),
			0,
			fb,
			nullptr
		});

		auto config = self->_device->createConfiguration();
		auto valid = config->capture(assignments);
		assert(valid);
		config->commit();

		self->_retirePageFlip(std::move(config), req.drm_cookie());
			
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_DIRTYFB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto obj = self->_device->findObject(req.drm_fb_id());
		assert(obj);
		auto fb = obj->asFrameBuffer();
		assert(fb);
		fb->notifyDirty();

		resp.set_error(managarm::fs::Errors::SUCCESS);
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_CURSOR) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;
	
		auto crtc_obj = self->_device->findObject(req.drm_crtc_id());
		assert(crtc_obj);
		auto crtc = crtc_obj->asCrtc();

		auto cursor_plane = crtc->cursorPlane();

		if (cursor_plane == nullptr) {
			resp.set_error(managarm::fs::Errors::NO_BACKING_DEVICE);
			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			co_return;
		}

		std::vector<Assignment> assignments;
		if (req.drm_flags() == DRM_MODE_CURSOR_BO) {
			resp.set_error(managarm::fs::Errors::SUCCESS);
			auto bo = self->resolveHandle(req.drm_handle());
			auto width = req.drm_width();
			auto height = req.drm_height();
			
			assignments.push_back(Assignment{ 
				cursor_plane->sharedModeObject(),
				self->_device->srcWProperty(),
				width,
				nullptr,
				nullptr
			});

			assignments.push_back(Assignment{
				cursor_plane->sharedModeObject(),
				self->_device->srcHProperty(),
				height,
				nullptr,
				nullptr
			});

			if (bo) {	
				auto fb = self->_device->createFrameBuffer(bo->sharedBufferObject(), width, height, DRM_FORMAT_ARGB8888, width * 4);
				assert(fb);
				assignments.push_back(Assignment{ 
					cursor_plane->sharedModeObject(),
					self->_device->fbIdProperty(),
					0,
					fb,
					nullptr
				});
			} else {
				assignments.push_back(Assignment{ 
					cursor_plane->sharedModeObject(),
					self->_device->fbIdProperty(),
					0,
					nullptr,
					nullptr
				});
			}
		}else if (req.drm_flags() == DRM_MODE_CURSOR_MOVE) {
			resp.set_error(managarm::fs::Errors::SUCCESS);
			auto x = req.drm_x();
			auto y = req.drm_y();

			assignments.push_back(Assignment{
				cursor_plane->sharedModeObject(),
				self->_device->crtcXProperty(),
				x,
				nullptr,
				nullptr
			});

			assignments.push_back(Assignment{
				cursor_plane->sharedModeObject(),
				self->_device->crtcYProperty(),
				y,
				nullptr,
				nullptr
			});
		}else{
			printf("\e[35mcore/drm: invalid request whilst handling DRM_IOCTL_MODE_CURSOR\e[39m\n");
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}

		auto config = self->_device->createConfiguration();
		auto valid = config->capture(assignments);
		assert(valid);
		config->commit();

		co_await config->waitForCompletion();

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
		helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_DESTROY_DUMB){
		self->_buffers.erase(req.drm_handle());

		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else{
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		std::cout << "\e[31m" "core/drm: Unknown ioctl() with ID "
				<< req.command() << "\e[39m" << std::endl;

		resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollWaitResult>>
drm_core::File::pollWait(void *object, uint64_t sequence, int mask,
		async::cancellation_token cancellation) {
	auto self = static_cast<drm_core::File *>(object);

	if(sequence > self->_eventSequence)
		co_return protocols::fs::Error::illegalArguments;

	// Wait until we surpass the input sequence.
	while(sequence == self->_eventSequence)
		co_await self->_eventBell.async_wait();

	co_return protocols::fs::PollWaitResult{self->_eventSequence,
			self->_eventSequence > 0 ? EPOLLIN : 0};
}

async::result<frg::expected<protocols::fs::Error, protocols::fs::PollStatusResult>>
drm_core::File::pollStatus(void *object) {
	auto self = static_cast<drm_core::File *>(object);

	int s = 0;
	if(!self->_pendingEvents.empty())
		s |= EPOLLIN;

	co_return protocols::fs::PollStatusResult{self->_eventSequence, s};
}

async::detached
drm_core::File::_retirePageFlip(std::unique_ptr<drm_core::Configuration> config,
			uint64_t cookie) {
	co_await config->waitForCompletion();

	Event event;
	event.cookie = cookie;
	postEvent(event);
}

namespace drm_core {

static constexpr auto defaultFileOperations = protocols::fs::FileOperations{
	.read = &File::read,
	.accessMemory = &File::accessMemory,
	.ioctl = &File::ioctl,
	.pollWait = &File::pollWait,
	.pollStatus = &File::pollStatus
};

async::detached serveDrmDevice(std::shared_ptr<drm_core::Device> device,
		helix::UniqueLane lane) {
	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		co_await header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();
		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			if(req.flags() & ~(managarm::fs::OpenFlags::OF_NONBLOCK)) {
				helix::SendBuffer send_resp;

				std::cout << "\e[31m" "core/drm: Illegal flags " << req.flags()
						<< " for DEV_OPEN" "\e[39m" << std::endl;

				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}

			helix::SendBuffer send_resp;
			helix::PushDescriptor push_pt;
			helix::PushDescriptor push_page;

			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			auto file = smarter::make_shared<drm_core::File>(device);

			if(req.flags() & managarm::fs::OpenFlags::OF_NONBLOCK)
				file->setBlocking(false);

			async::detach(protocols::fs::servePassthrough(
					std::move(local_lane), file, &defaultFileOperations));

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_caps(managarm::fs::FileCaps::FC_STATUS_PAGE);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&push_pt, remote_lane, kHelItemChain),
					helix::action(&push_page, file->statusPageMemory()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_pt.error());
			HEL_CHECK(push_page.error());
		}else{
			throw std::runtime_error("Invalid request in serveDevice()");
		}
	}
}

} // namespace core_drm

// ----------------------------------------------------------------
// Functions
// ----------------------------------------------------------------

uint32_t drm_core::convertLegacyFormat(uint32_t bpp, uint32_t depth) {
	switch(bpp) {
		case 8:
			assert(depth == 8);
			return DRM_FORMAT_C8;
		
		case 16:
			assert(depth == 15 || depth == 16);
			if(depth == 15) {
				return DRM_FORMAT_XRGB1555;
			}else {
				return DRM_FORMAT_RGB565;
			}
		
		case 24:
			assert(depth == 24);
			return DRM_FORMAT_RGB888;

		case 32:
			assert(depth == 24 || depth == 30 || depth == 32);
			if(depth == 24) {
				return DRM_FORMAT_XRGB8888;
			}else if(depth == 30) {
				return DRM_FORMAT_XRGB2101010;
			}else {
				return DRM_FORMAT_ARGB8888;
			}

		default:
			throw std::runtime_error("Bad BPP");
	}
}

std::optional<drm_core::FormatInfo> drm_core::getFormatInfo(uint32_t fourcc) {
	switch(fourcc) {
		case(DRM_FORMAT_C8): return FormatInfo{1};
		case(DRM_FORMAT_XRGB1555): return FormatInfo{2};
		case(DRM_FORMAT_RGB565): return FormatInfo{2};
		case(DRM_FORMAT_RGB888): return FormatInfo{3};
		case(DRM_FORMAT_XRGB8888): return FormatInfo{4};
		case(DRM_FORMAT_XRGB2101010): return FormatInfo{4};
		case(DRM_FORMAT_ARGB8888): return FormatInfo{4};
		default: return std::nullopt;
	}
}

drm_mode_modeinfo drm_core::makeModeInfo(const char *name, uint32_t type,
		uint32_t clock, unsigned int hdisplay, unsigned int hsync_start,
		unsigned int hsync_end, unsigned int htotal, unsigned int hskew,
		unsigned int vdisplay, unsigned int vsync_start, unsigned int vsync_end,
		unsigned int vtotal, unsigned int vscan, uint32_t flags) {
	drm_mode_modeinfo mode_info;
	mode_info.clock = clock;
	mode_info.hdisplay = hdisplay;
	mode_info.hsync_start = hsync_start;
	mode_info.hsync_end = hsync_end;
	mode_info.htotal = htotal;
	mode_info.hskew = hskew;
	mode_info.vdisplay = vdisplay;
	mode_info.vsync_start = vsync_start;
	mode_info.vsync_end = vsync_end;
	mode_info.vtotal = vtotal;
	mode_info.vscan = vscan;
	mode_info.flags = flags;
	mode_info.type = type;
	strcpy(mode_info.name, name);
	return mode_info;
};

void drm_core::addDmtModes(std::vector<drm_mode_modeinfo> &supported_modes,
		unsigned int max_width, unsigned max_height) {
	drm_mode_modeinfo modes[] = {
	/* 0x01 - 640x350@85Hz */
	makeModeInfo("640x350", DRM_MODE_TYPE_DRIVER, 31500, 640, 672,
		   736, 832, 0, 350, 382, 385, 445, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x02 - 640x400@85Hz */
	makeModeInfo("640x400", DRM_MODE_TYPE_DRIVER, 31500, 640, 672,
		   736, 832, 0, 400, 401, 404, 445, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x03 - 720x400@85Hz */
	makeModeInfo("720x400", DRM_MODE_TYPE_DRIVER, 35500, 720, 756,
		   828, 936, 0, 400, 401, 404, 446, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x04 - 640x480@60Hz */
	makeModeInfo("640x480", DRM_MODE_TYPE_DRIVER, 25175, 640, 656,
		   752, 800, 0, 480, 490, 492, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x05 - 640x480@72Hz */
	makeModeInfo("640x480", DRM_MODE_TYPE_DRIVER, 31500, 640, 664,
		   704, 832, 0, 480, 489, 492, 520, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x06 - 640x480@75Hz */
	makeModeInfo("640x480", DRM_MODE_TYPE_DRIVER, 31500, 640, 656,
		   720, 840, 0, 480, 481, 484, 500, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x07 - 640x480@85Hz */
	makeModeInfo("640x480", DRM_MODE_TYPE_DRIVER, 36000, 640, 696,
		   752, 832, 0, 480, 481, 484, 509, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x08 - 800x600@56Hz */
	makeModeInfo("800x600", DRM_MODE_TYPE_DRIVER, 36000, 800, 824,
		   896, 1024, 0, 600, 601, 603, 625, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x09 - 800x600@60Hz */
	makeModeInfo("800x600", DRM_MODE_TYPE_DRIVER, 40000, 800, 840,
		   968, 1056, 0, 600, 601, 605, 628, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x0a - 800x600@72Hz */
	makeModeInfo("800x600", DRM_MODE_TYPE_DRIVER, 50000, 800, 856,
		   976, 1040, 0, 600, 637, 643, 666, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x0b - 800x600@75Hz */
	makeModeInfo("800x600", DRM_MODE_TYPE_DRIVER, 49500, 800, 816,
		   896, 1056, 0, 600, 601, 604, 625, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x0c - 800x600@85Hz */
	makeModeInfo("800x600", DRM_MODE_TYPE_DRIVER, 56250, 800, 832,
		   896, 1048, 0, 600, 601, 604, 631, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x0d - 800x600@120Hz RB */
	makeModeInfo("800x600", DRM_MODE_TYPE_DRIVER, 73250, 800, 848,
		   880, 960, 0, 600, 603, 607, 636, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x0e - 848x480@60Hz */
	makeModeInfo("848x480", DRM_MODE_TYPE_DRIVER, 33750, 848, 864,
		   976, 1088, 0, 480, 486, 494, 517, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x0f - 1024x768@43Hz, interlace */
	makeModeInfo("1024x768i", DRM_MODE_TYPE_DRIVER, 44900, 1024, 1032,
		   1208, 1264, 0, 768, 768, 776, 817, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC |
		   DRM_MODE_FLAG_INTERLACE),
	/* 0x10 - 1024x768@60Hz */
	makeModeInfo("1024x768", DRM_MODE_TYPE_DRIVER, 65000, 1024, 1048,
		   1184, 1344, 0, 768, 771, 777, 806, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x11 - 1024x768@70Hz */
	makeModeInfo("1024x768", DRM_MODE_TYPE_DRIVER, 75000, 1024, 1048,
		   1184, 1328, 0, 768, 771, 777, 806, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x12 - 1024x768@75Hz */
	makeModeInfo("1024x768", DRM_MODE_TYPE_DRIVER, 78750, 1024, 1040,
		   1136, 1312, 0, 768, 769, 772, 800, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x13 - 1024x768@85Hz */
	makeModeInfo("1024x768", DRM_MODE_TYPE_DRIVER, 94500, 1024, 1072,
		   1168, 1376, 0, 768, 769, 772, 808, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x14 - 1024x768@120Hz RB */
	makeModeInfo("1024x768", DRM_MODE_TYPE_DRIVER, 115500, 1024, 1072,
		   1104, 1184, 0, 768, 771, 775, 813, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x15 - 1152x864@75Hz */
	makeModeInfo("1152x864", DRM_MODE_TYPE_DRIVER, 108000, 1152, 1216,
		   1344, 1600, 0, 864, 865, 868, 900, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x55 - 1280x720@60Hz */
	makeModeInfo("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 1390,
		   1430, 1650, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x16 - 1280x768@60Hz RB */
	makeModeInfo("1280x768", DRM_MODE_TYPE_DRIVER, 68250, 1280, 1328,
		   1360, 1440, 0, 768, 771, 778, 790, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x17 - 1280x768@60Hz */
	makeModeInfo("1280x768", DRM_MODE_TYPE_DRIVER, 79500, 1280, 1344,
		   1472, 1664, 0, 768, 771, 778, 798, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x18 - 1280x768@75Hz */
	makeModeInfo("1280x768", DRM_MODE_TYPE_DRIVER, 102250, 1280, 1360,
		   1488, 1696, 0, 768, 771, 778, 805, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x19 - 1280x768@85Hz */
	makeModeInfo("1280x768", DRM_MODE_TYPE_DRIVER, 117500, 1280, 1360,
		   1496, 1712, 0, 768, 771, 778, 809, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x1a - 1280x768@120Hz RB */
	makeModeInfo("1280x768", DRM_MODE_TYPE_DRIVER, 140250, 1280, 1328,
		   1360, 1440, 0, 768, 771, 778, 813, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x1b - 1280x800@60Hz RB */
	makeModeInfo("1280x800", DRM_MODE_TYPE_DRIVER, 71000, 1280, 1328,
		   1360, 1440, 0, 800, 803, 809, 823, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x1c - 1280x800@60Hz */
	makeModeInfo("1280x800", DRM_MODE_TYPE_DRIVER, 83500, 1280, 1352,
		   1480, 1680, 0, 800, 803, 809, 831, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x1d - 1280x800@75Hz */
	makeModeInfo("1280x800", DRM_MODE_TYPE_DRIVER, 106500, 1280, 1360,
		   1488, 1696, 0, 800, 803, 809, 838, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x1e - 1280x800@85Hz */
	makeModeInfo("1280x800", DRM_MODE_TYPE_DRIVER, 122500, 1280, 1360,
		   1496, 1712, 0, 800, 803, 809, 843, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x1f - 1280x800@120Hz RB */
	makeModeInfo("1280x800", DRM_MODE_TYPE_DRIVER, 146250, 1280, 1328,
		   1360, 1440, 0, 800, 803, 809, 847, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x20 - 1280x960@60Hz */
	makeModeInfo("1280x960", DRM_MODE_TYPE_DRIVER, 108000, 1280, 1376,
		   1488, 1800, 0, 960, 961, 964, 1000, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x21 - 1280x960@85Hz */
	makeModeInfo("1280x960", DRM_MODE_TYPE_DRIVER, 148500, 1280, 1344,
		   1504, 1728, 0, 960, 961, 964, 1011, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x22 - 1280x960@120Hz RB */
	makeModeInfo("1280x960", DRM_MODE_TYPE_DRIVER, 175500, 1280, 1328,
		   1360, 1440, 0, 960, 963, 967, 1017, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x23 - 1280x1024@60Hz */
	makeModeInfo("1280x1024", DRM_MODE_TYPE_DRIVER, 108000, 1280, 1328,
		   1440, 1688, 0, 1024, 1025, 1028, 1066, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x24 - 1280x1024@75Hz */
	makeModeInfo("1280x1024", DRM_MODE_TYPE_DRIVER, 135000, 1280, 1296,
		   1440, 1688, 0, 1024, 1025, 1028, 1066, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x25 - 1280x1024@85Hz */
	makeModeInfo("1280x1024", DRM_MODE_TYPE_DRIVER, 157500, 1280, 1344,
		   1504, 1728, 0, 1024, 1025, 1028, 1072, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x26 - 1280x1024@120Hz RB */
	makeModeInfo("1280x1024", DRM_MODE_TYPE_DRIVER, 187250, 1280, 1328,
		   1360, 1440, 0, 1024, 1027, 1034, 1084, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x27 - 1360x768@60Hz */
	makeModeInfo("1360x768", DRM_MODE_TYPE_DRIVER, 85500, 1360, 1424,
		   1536, 1792, 0, 768, 771, 777, 795, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x28 - 1360x768@120Hz RB */
	makeModeInfo("1360x768", DRM_MODE_TYPE_DRIVER, 148250, 1360, 1408,
		   1440, 1520, 0, 768, 771, 776, 813, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x51 - 1366x768@60Hz */
	makeModeInfo("1366x768", DRM_MODE_TYPE_DRIVER, 85500, 1366, 1436,
		   1579, 1792, 0, 768, 771, 774, 798, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x56 - 1366x768@60Hz */
	makeModeInfo("1366x768", DRM_MODE_TYPE_DRIVER, 72000, 1366, 1380,
		   1436, 1500, 0, 768, 769, 772, 800, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x29 - 1400x1050@60Hz RB */
	makeModeInfo("1400x1050", DRM_MODE_TYPE_DRIVER, 101000, 1400, 1448,
		   1480, 1560, 0, 1050, 1053, 1057, 1080, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x2a - 1400x1050@60Hz */
	makeModeInfo("1400x1050", DRM_MODE_TYPE_DRIVER, 121750, 1400, 1488,
		   1632, 1864, 0, 1050, 1053, 1057, 1089, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x2b - 1400x1050@75Hz */
	makeModeInfo("1400x1050", DRM_MODE_TYPE_DRIVER, 156000, 1400, 1504,
		   1648, 1896, 0, 1050, 1053, 1057, 1099, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x2c - 1400x1050@85Hz */
	makeModeInfo("1400x1050", DRM_MODE_TYPE_DRIVER, 179500, 1400, 1504,
		   1656, 1912, 0, 1050, 1053, 1057, 1105, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x2d - 1400x1050@120Hz RB */
	makeModeInfo("1400x1050", DRM_MODE_TYPE_DRIVER, 208000, 1400, 1448,
		   1480, 1560, 0, 1050, 1053, 1057, 1112, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x2e - 1440x900@60Hz RB */
	makeModeInfo("1440x900", DRM_MODE_TYPE_DRIVER, 88750, 1440, 1488,
		   1520, 1600, 0, 900, 903, 909, 926, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x2f - 1440x900@60Hz */
	makeModeInfo("1440x900", DRM_MODE_TYPE_DRIVER, 106500, 1440, 1520,
		   1672, 1904, 0, 900, 903, 909, 934, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x30 - 1440x900@75Hz */
	makeModeInfo("1440x900", DRM_MODE_TYPE_DRIVER, 136750, 1440, 1536,
		   1688, 1936, 0, 900, 903, 909, 942, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x31 - 1440x900@85Hz */
	makeModeInfo("1440x900", DRM_MODE_TYPE_DRIVER, 157000, 1440, 1544,
		   1696, 1952, 0, 900, 903, 909, 948, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x32 - 1440x900@120Hz RB */
	makeModeInfo("1440x900", DRM_MODE_TYPE_DRIVER, 182750, 1440, 1488,
		   1520, 1600, 0, 900, 903, 909, 953, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x53 - 1600x900@60Hz */
	makeModeInfo("1600x900", DRM_MODE_TYPE_DRIVER, 108000, 1600, 1624,
		   1704, 1800, 0, 900, 901, 904, 1000, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x33 - 1600x1200@60Hz */
	makeModeInfo("1600x1200", DRM_MODE_TYPE_DRIVER, 162000, 1600, 1664,
		   1856, 2160, 0, 1200, 1201, 1204, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x34 - 1600x1200@65Hz */
	makeModeInfo("1600x1200", DRM_MODE_TYPE_DRIVER, 175500, 1600, 1664,
		   1856, 2160, 0, 1200, 1201, 1204, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x35 - 1600x1200@70Hz */
	makeModeInfo("1600x1200", DRM_MODE_TYPE_DRIVER, 189000, 1600, 1664,
		   1856, 2160, 0, 1200, 1201, 1204, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x36 - 1600x1200@75Hz */
	makeModeInfo("1600x1200", DRM_MODE_TYPE_DRIVER, 202500, 1600, 1664,
		   1856, 2160, 0, 1200, 1201, 1204, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x37 - 1600x1200@85Hz */
	makeModeInfo("1600x1200", DRM_MODE_TYPE_DRIVER, 229500, 1600, 1664,
		   1856, 2160, 0, 1200, 1201, 1204, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x38 - 1600x1200@120Hz RB */
	makeModeInfo("1600x1200", DRM_MODE_TYPE_DRIVER, 268250, 1600, 1648,
		   1680, 1760, 0, 1200, 1203, 1207, 1271, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x39 - 1680x1050@60Hz RB */
	makeModeInfo("1680x1050", DRM_MODE_TYPE_DRIVER, 119000, 1680, 1728,
		   1760, 1840, 0, 1050, 1053, 1059, 1080, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x3a - 1680x1050@60Hz */
	makeModeInfo("1680x1050", DRM_MODE_TYPE_DRIVER, 146250, 1680, 1784,
		   1960, 2240, 0, 1050, 1053, 1059, 1089, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x3b - 1680x1050@75Hz */
	makeModeInfo("1680x1050", DRM_MODE_TYPE_DRIVER, 187000, 1680, 1800,
		   1976, 2272, 0, 1050, 1053, 1059, 1099, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x3c - 1680x1050@85Hz */
	makeModeInfo("1680x1050", DRM_MODE_TYPE_DRIVER, 214750, 1680, 1808,
		   1984, 2288, 0, 1050, 1053, 1059, 1105, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x3d - 1680x1050@120Hz RB */
	makeModeInfo("1680x1050", DRM_MODE_TYPE_DRIVER, 245500, 1680, 1728,
		   1760, 1840, 0, 1050, 1053, 1059, 1112, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x3e - 1792x1344@60Hz */
	makeModeInfo("1792x1344", DRM_MODE_TYPE_DRIVER, 204750, 1792, 1920,
		   2120, 2448, 0, 1344, 1345, 1348, 1394, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x3f - 1792x1344@75Hz */
	makeModeInfo("1792x1344", DRM_MODE_TYPE_DRIVER, 261000, 1792, 1888,
		   2104, 2456, 0, 1344, 1345, 1348, 1417, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x40 - 1792x1344@120Hz RB */
	makeModeInfo("1792x1344", DRM_MODE_TYPE_DRIVER, 333250, 1792, 1840,
		   1872, 1952, 0, 1344, 1347, 1351, 1423, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x41 - 1856x1392@60Hz */
	makeModeInfo("1856x1392", DRM_MODE_TYPE_DRIVER, 218250, 1856, 1952,
		   2176, 2528, 0, 1392, 1393, 1396, 1439, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x42 - 1856x1392@75Hz */
	makeModeInfo("1856x1392", DRM_MODE_TYPE_DRIVER, 288000, 1856, 1984,
		   2208, 2560, 0, 1392, 1393, 1396, 1500, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x43 - 1856x1392@120Hz RB */
	makeModeInfo("1856x1392", DRM_MODE_TYPE_DRIVER, 356500, 1856, 1904,
		   1936, 2016, 0, 1392, 1395, 1399, 1474, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x52 - 1920x1080@60Hz */
	makeModeInfo("1920x1080", DRM_MODE_TYPE_DRIVER, 148500, 1920, 2008,
		   2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x44 - 1920x1200@60Hz RB */
	makeModeInfo("1920x1200", DRM_MODE_TYPE_DRIVER, 154000, 1920, 1968,
		   2000, 2080, 0, 1200, 1203, 1209, 1235, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x45 - 1920x1200@60Hz */
	makeModeInfo("1920x1200", DRM_MODE_TYPE_DRIVER, 193250, 1920, 2056,
		   2256, 2592, 0, 1200, 1203, 1209, 1245, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x46 - 1920x1200@75Hz */
	makeModeInfo("1920x1200", DRM_MODE_TYPE_DRIVER, 245250, 1920, 2056,
		   2264, 2608, 0, 1200, 1203, 1209, 1255, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x47 - 1920x1200@85Hz */
	makeModeInfo("1920x1200", DRM_MODE_TYPE_DRIVER, 281250, 1920, 2064,
		   2272, 2624, 0, 1200, 1203, 1209, 1262, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x48 - 1920x1200@120Hz RB */
	makeModeInfo("1920x1200", DRM_MODE_TYPE_DRIVER, 317000, 1920, 1968,
		   2000, 2080, 0, 1200, 1203, 1209, 1271, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x49 - 1920x1440@60Hz */
	makeModeInfo("1920x1440", DRM_MODE_TYPE_DRIVER, 234000, 1920, 2048,
		   2256, 2600, 0, 1440, 1441, 1444, 1500, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x4a - 1920x1440@75Hz */
	makeModeInfo("1920x1440", DRM_MODE_TYPE_DRIVER, 297000, 1920, 2064,
		   2288, 2640, 0, 1440, 1441, 1444, 1500, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x4b - 1920x1440@120Hz RB */
	makeModeInfo("1920x1440", DRM_MODE_TYPE_DRIVER, 380500, 1920, 1968,
		   2000, 2080, 0, 1440, 1443, 1447, 1525, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x54 - 2048x1152@60Hz */
	makeModeInfo("2048x1152", DRM_MODE_TYPE_DRIVER, 162000, 2048, 2074,
		   2154, 2250, 0, 1152, 1153, 1156, 1200, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x4c - 2560x1600@60Hz RB */
	makeModeInfo("2560x1600", DRM_MODE_TYPE_DRIVER, 268500, 2560, 2608,
		   2640, 2720, 0, 1600, 1603, 1609, 1646, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x4d - 2560x1600@60Hz */
	makeModeInfo("2560x1600", DRM_MODE_TYPE_DRIVER, 348500, 2560, 2752,
		   3032, 3504, 0, 1600, 1603, 1609, 1658, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x4e - 2560x1600@75Hz */
	makeModeInfo("2560x1600", DRM_MODE_TYPE_DRIVER, 443250, 2560, 2768,
		   3048, 3536, 0, 1600, 1603, 1609, 1672, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x4f - 2560x1600@85Hz */
	makeModeInfo("2560x1600", DRM_MODE_TYPE_DRIVER, 505250, 2560, 2768,
		   3048, 3536, 0, 1600, 1603, 1609, 1682, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	/* 0x50 - 2560x1600@120Hz RB */
	makeModeInfo("2560x1600", DRM_MODE_TYPE_DRIVER, 552750, 2560, 2608,
		   2640, 2720, 0, 1600, 1603, 1609, 1694, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x57 - 4096x2160@60Hz RB */
	makeModeInfo("4096x2160", DRM_MODE_TYPE_DRIVER, 556744, 4096, 4104,
		   4136, 4176, 0, 2160, 2208, 2216, 2222, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC),
	/* 0x58 - 4096x2160@59.94Hz RB */
	makeModeInfo("4096x2160", DRM_MODE_TYPE_DRIVER, 556188, 4096, 4104,
		   4136, 4176, 0, 2160, 2208, 2216, 2222, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC)
	};
	size_t size = sizeof(modes) / sizeof(drm_mode_modeinfo);

	for(size_t i = 0; i < size; i++) {
		if(modes[i].hdisplay <= max_width && modes[i].vdisplay <= max_height)
			supported_modes.push_back(modes[i]);
	}
}

