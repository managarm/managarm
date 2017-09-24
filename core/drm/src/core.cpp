
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

#include "fs.pb.h"
#include "core/drm/core.hpp"

// ----------------------------------------------------------------
// Device
// ----------------------------------------------------------------

void drm_core::Device::setupCrtc(std::shared_ptr<drm_core::Crtc> crtc) {
	crtc->index = _crtcs.size();
	_crtcs.push_back(crtc);
}

void drm_core::Device::setupEncoder(std::shared_ptr<drm_core::Encoder> encoder) {
	encoder->index = _encoders.size();
	_encoders.push_back(encoder);
}

void drm_core::Device::attachConnector(std::shared_ptr<drm_core::Connector> connector) {
	_connectors.push_back(connector);
}

const std::vector<std::shared_ptr<drm_core::Crtc>> &drm_core::Device::getCrtcs() {
	return _crtcs;
}

const std::vector<std::shared_ptr<drm_core::Encoder>> &drm_core::Device::getEncoders() {
	return _encoders;
}

const std::vector<std::shared_ptr<drm_core::Connector>> &drm_core::Device::getConnectors() {
	return _connectors;
}

void drm_core::Device::registerObject(std::shared_ptr<drm_core::ModeObject> object) {
	_objects.insert({object->id(), object});
}

drm_core::ModeObject *drm_core::Device::findObject(uint32_t id) {
	auto it = _objects.find(id);
	if(it == _objects.end())
		return nullptr;
	return it->second.get();
}

uint64_t drm_core::Device::installMapping(drm_core::BufferObject *bo) {
	auto address = _mappingAllocator.allocate(bo->getSize());
	_mappings.insert({address, bo});
	return address;
}
	
std::pair<uint64_t, drm_core::BufferObject *> drm_core::Device::findMapping(uint64_t offset) {
	auto it = _mappings.upper_bound(offset);
	if(it == _mappings.begin())
		throw std::runtime_error("Mapping does not exist!");
	
	it--;
	return *it;
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

void drm_core::File::attachFrameBuffer(std::shared_ptr<drm_core::FrameBuffer> frame_buffer) {
	_frameBuffers.push_back(frame_buffer);
}
	
const std::vector<std::shared_ptr<drm_core::FrameBuffer>> &drm_core::File::getFrameBuffers() {
	return _frameBuffers;
}
	
uint32_t drm_core::File::createHandle(std::shared_ptr<BufferObject> bo) {
	auto handle = _allocator.allocate();
	_buffers.insert({handle, bo});
	return handle;
}
	
drm_core::BufferObject *drm_core::File::resolveHandle(uint32_t handle) {
	auto it = _buffers.find(handle);
	if(it == _buffers.end())
		return nullptr;
	return it->second.get();
};

async::result<size_t> drm_core::File::read(std::shared_ptr<void> object, void *buffer, size_t length) {
	throw std::runtime_error("read() not implemented");
}

COFIBER_ROUTINE(async::result<protocols::fs::AccessMemoryResult>, drm_core::File::accessMemory(std::shared_ptr<void> object,
		uint64_t offset, size_t), ([=] {
	auto self = std::static_pointer_cast<drm_core::File>(object);
	auto mapping = self->_device->findMapping(offset);
	auto mem = mapping.second->getMemory();
	COFIBER_RETURN(std::make_pair(mem.first, mem.second + (offset - mapping.first)));
}))

COFIBER_ROUTINE(async::result<void>, drm_core::File::ioctl(std::shared_ptr<void> object, managarm::fs::CntRequest req,
		helix::UniqueLane conversation), ([object = std::move(object), req = std::move(req),
		conversation = std::move(conversation)] {
	auto self = std::static_pointer_cast<drm_core::File>(object);
	if(req.command() == DRM_IOCTL_GET_CAP) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;
		
		if(req.drm_capability() == DRM_CAP_DUMB_BUFFER) {
			resp.set_drm_value(1);
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}else{
			resp.set_drm_value(0);
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}
		
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
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
		COFIBER_AWAIT transmit.async_wait();
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
					conn->modeList().size() * sizeof(drm_mode_modeinfo)));
		COFIBER_AWAIT transmit.async_wait();
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
		COFIBER_AWAIT transmit.async_wait();
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
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_ADDFB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto bo = self->resolveHandle(req.drm_handle());
		assert(bo);
		auto buffer = bo->sharedBufferObject();
		
		auto fb = self->_device->createFrameBuffer(buffer, req.drm_width(), req.drm_height(),
				req.drm_bpp(), req.drm_pitch());
		self->attachFrameBuffer(fb);
		resp.set_drm_fb_id(fb->id());
		resp.set_error(managarm::fs::Errors::SUCCESS);
	
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
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
		COFIBER_AWAIT transmit.async_wait();
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
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_mode.error());
	}else if(req.command() == DRM_IOCTL_MODE_SETCRTC) {
		std::vector<char> mode_buffer;
		mode_buffer.resize(sizeof(drm_mode_modeinfo));

		helix::RecvBuffer recv_buffer;
		auto &&buff = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&recv_buffer, mode_buffer.data(), sizeof(drm_mode_modeinfo)));
		COFIBER_AWAIT buff.async_wait();
		HEL_CHECK(recv_buffer.error());
		
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;
	
		auto config = self->_device->createConfiguration();
	
		auto obj = self->_device->findObject(req.drm_crtc_id());
		assert(obj);
		auto crtc = obj->asCrtc();
		assert(crtc);
	
		std::vector<drm_core::Assignment> assignments;
		if(req.drm_mode_valid()) {
			auto mode_blob = std::make_shared<Blob>(std::move(mode_buffer));
			assignments.push_back(Assignment{ 
				crtc,
				&self->_device->modeIdProperty,
				0,
				nullptr,
				mode_blob
			});
			
			auto id = req.drm_fb_id();
			auto fb = self->_device->findObject(id);
			assert(fb);
			assignments.push_back(Assignment{ 
				crtc->primaryPlane(),
				&self->_device->fbIdProperty, 
				0,
				fb,
				nullptr
			});
		}else{
			std::vector<drm_core::Assignment> assignments;
			assignments.push_back(Assignment{ 
				crtc,
				&self->_device->modeIdProperty,
				0,
				nullptr,
				nullptr
			});
		}

		auto valid = config->capture(assignments);
		assert(valid);
		config->commit();
			
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else{
		throw std::runtime_error("Unknown ioctl() with ID" + std::to_string(req.command()));
	}
	COFIBER_RETURN();
}))

