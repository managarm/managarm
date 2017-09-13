
#include <assert.h>
#include <stdio.h>
#include <deque>
#include <experimental/optional>
#include <functional>
#include <iostream>
#include <memory>

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
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/server.hpp>

#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>

#include "bochs.hpp"
#include <fs.pb.h>

// ----------------------------------------------------------------
// Stuff that belongs in a DRM library.
// ----------------------------------------------------------------

// ----------------------------------------------------------------
// Device
// ----------------------------------------------------------------

void drm_backend::Device::setupCrtc(std::shared_ptr<drm_backend::Crtc> crtc) {
	crtc->index = _crtcs.size();
	_crtcs.push_back(crtc);
}

void drm_backend::Device::setupEncoder(std::shared_ptr<drm_backend::Encoder> encoder) {
	encoder->index = _encoders.size();
	_encoders.push_back(encoder);
}

void drm_backend::Device::attachConnector(std::shared_ptr<drm_backend::Connector> connector) {
	_connectors.push_back(connector);
}

const std::vector<std::shared_ptr<drm_backend::Crtc>> &drm_backend::Device::getCrtcs() {
	return _crtcs;
}

const std::vector<std::shared_ptr<drm_backend::Encoder>> &drm_backend::Device::getEncoders() {
	return _encoders;
}

const std::vector<std::shared_ptr<drm_backend::Connector>> &drm_backend::Device::getConnectors() {
	return _connectors;
}

void drm_backend::Device::registerObject(std::shared_ptr<drm_backend::Object> object) {
	_objects.insert({object->id(), object});
}

drm_backend::Object *drm_backend::Device::findObject(uint32_t id) {
	auto it = _objects.find(id);
	if(it == _objects.end())
		return nullptr;
	return it->second.get();
}

uint64_t drm_backend::Device::installMapping(drm_backend::BufferObject *bo) {
	auto address = _mappingAllocator.allocate(bo->getSize());
	_mappings.insert({address, bo});
	return address;
}
	
std::pair<uint64_t, drm_backend::BufferObject *> drm_backend::Device::findMapping(uint64_t offset) {
	auto it = _mappings.upper_bound(offset);
	if(it == _mappings.begin())
		throw std::runtime_error("Mapping does not exist!");
	
	it--;
	return *it;
}

void drm_backend::Device::setupMinDimensions(uint32_t width, uint32_t height) {
	_minWidth = width;
	_minHeight = height;
}

void drm_backend::Device::setupMaxDimensions(uint32_t width, uint32_t height) {
	_maxWidth = width;
	_maxHeight = height;
}

uint32_t drm_backend::Device::getMinWidth() {
	return _minWidth;
}

uint32_t drm_backend::Device::getMaxWidth() {
	return _maxWidth;
}

uint32_t drm_backend::Device::getMinHeight() {
	return _minHeight;
}

uint32_t drm_backend::Device::getMaxHeight() {
	return _maxHeight;
}
	
void drm_backend::Device::setupPhysicalDimensions(uint32_t width, uint32_t height) {
	_physicalWidth = width;
	_physicalHeight = height;
}

uint32_t drm_backend::Device::getPhysicalWidth() {
	return _physicalWidth;
}

uint32_t drm_backend::Device::getPhysicalHeight() {
	return _physicalHeight;
}

void drm_backend::Device::setupSubpixel(uint32_t subpixel) {
	_subpixel = subpixel;
}

uint32_t drm_backend::Device::getSubpixel() {
	return _subpixel;
}

uint32_t drm_backend::Device::connectorType() {
	return _connectorType;
}
	
// ----------------------------------------------------------------
// BufferObject
// ----------------------------------------------------------------

void drm_backend::BufferObject::setupMapping(uint64_t mapping) {
	_mapping = mapping;
}

uint64_t drm_backend::BufferObject::getMapping() {
	return _mapping;
}

// ----------------------------------------------------------------
// Encoder
// ----------------------------------------------------------------

drm_backend::Crtc *drm_backend::Encoder::currentCrtc() {
	return _currentCrtc;
}

void drm_backend::Encoder::setCurrentCrtc(drm_backend::Crtc *crtc) {
	_currentCrtc = crtc;
}
	
void drm_backend::Encoder::setupEncoderType(uint32_t type) {
	_encoderType = type;
}
	
uint32_t drm_backend::Encoder::getEncoderType() {
	return _encoderType;
}
	
void drm_backend::Encoder::setupPossibleCrtcs(std::vector<drm_backend::Crtc *> crtcs) {
	_possibleCrtcs = crtcs;
}
	
const std::vector<drm_backend::Crtc *> &drm_backend::Encoder::getPossibleCrtcs() {
	return _possibleCrtcs;
}
	
void drm_backend::Encoder::setupPossibleClones(std::vector<drm_backend::Encoder *> clones) {
	_possibleClones = clones;
}
	
const std::vector<drm_backend::Encoder *> &drm_backend::Encoder::getPossibleClones() {
	return _possibleClones;
}

// ----------------------------------------------------------------
// Object
// ----------------------------------------------------------------

uint32_t drm_backend::Object::id() {
	return _id;
}

drm_backend::Encoder *drm_backend::Object::asEncoder() {
	return nullptr;
}

drm_backend::Connector *drm_backend::Object::asConnector() {
	return nullptr;
}

drm_backend::Crtc *drm_backend::Object::asCrtc() {
	return nullptr;
}

drm_backend::FrameBuffer *drm_backend::Object::asFrameBuffer() {
	return nullptr;
}

drm_backend::Plane *drm_backend::Object::asPlane() {
	return nullptr;
}

// ----------------------------------------------------------------
// Crtc
// ----------------------------------------------------------------

std::shared_ptr<drm_backend::Blob> drm_backend::Crtc::currentMode() {
	return _curMode;
}
	
void drm_backend::Crtc::setCurrentMode(std::shared_ptr<drm_backend::Blob> mode) {
	_curMode = mode;
}

// ----------------------------------------------------------------
// Connector
// ----------------------------------------------------------------

const std::vector<drm_mode_modeinfo> &drm_backend::Connector::modeList() {
	return _modeList;
}

void drm_backend::Connector::setModeList(std::vector<drm_mode_modeinfo> mode_list) {
	_modeList = mode_list;
}
	
void drm_backend::Connector::setCurrentStatus(uint32_t status) {
	_currentStatus = status;
}
	
void drm_backend::Connector::setCurrentEncoder(drm_backend::Encoder *encoder) {
	_currentEncoder = encoder;
}
	
drm_backend::Encoder *drm_backend::Connector::currentEncoder() {
	return _currentEncoder;
}

uint32_t drm_backend::Connector::getCurrentStatus() {
	return _currentStatus;
}

// ----------------------------------------------------------------
// Blob
// ----------------------------------------------------------------

size_t drm_backend::Blob::size() {
	return _data.size();
}
	
const void *drm_backend::Blob::data() {
	return _data.data();
}

// ----------------------------------------------------------------
// File
// ----------------------------------------------------------------

void drm_backend::File::attachFrameBuffer(std::shared_ptr<drm_backend::FrameBuffer> frame_buffer) {
	_frameBuffers.push_back(frame_buffer);
}
	
const std::vector<std::shared_ptr<drm_backend::FrameBuffer>> &drm_backend::File::getFrameBuffers() {
	return _frameBuffers;
}
	
uint32_t drm_backend::File::createHandle(std::shared_ptr<BufferObject> bo) {
	auto handle = _allocator.allocate();
	_buffers.insert({handle, bo});
	return handle;
}
	
drm_backend::BufferObject *drm_backend::File::resolveHandle(uint32_t handle) {
	auto it = _buffers.find(handle);
	if(it == _buffers.end())
		return nullptr;
	return it->second.get();
};

async::result<size_t> drm_backend::File::read(std::shared_ptr<void> object, void *buffer, size_t length) {
	throw std::runtime_error("read() not implemented");
}

COFIBER_ROUTINE(async::result<protocols::fs::AccessMemoryResult>, drm_backend::File::accessMemory(std::shared_ptr<void> object,
		uint64_t offset, size_t), ([=] {
	auto self = std::static_pointer_cast<drm_backend::File>(object);
	auto mapping = self->_device->findMapping(offset);
	auto mem = mapping.second->getMemory();
	COFIBER_RETURN(std::make_pair(mem.first, mem.second + (offset - mapping.first)));
}))

COFIBER_ROUTINE(async::result<void>, drm_backend::File::ioctl(std::shared_ptr<void> object, managarm::fs::CntRequest req,
		helix::UniqueLane conversation), ([object = std::move(object), req = std::move(req),
		conversation = std::move(conversation)] {
	auto self = std::static_pointer_cast<drm_backend::File>(object);
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
			resp.add_drm_crtc_ids(crtcs[i]->asObject()->id());
		}
			
		auto &encoders = self->_device->getEncoders();
		for(int i = 0; i < encoders.size(); i++) {
			resp.add_drm_encoder_ids(encoders[i]->asObject()->id());
		}
	
		auto &connectors = self->_device->getConnectors();
		for(int i = 0; i < connectors.size(); i++) {
			resp.add_drm_connector_ids(connectors[i]->asObject()->id());
		}
		
		auto &fbs = self->getFrameBuffers();
		for(int i = 0; i < fbs.size(); i++) {
			resp.add_drm_fb_ids(fbs[i]->asObject()->id());
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
		
		auto psbl_enc = conn->possibleEncoders();
		for(int i = 0; i < psbl_enc.size(); i++) { 
			resp.add_drm_encoders(psbl_enc[i]->asObject()->id());
		}

		resp.set_drm_encoder_id(conn->currentEncoder()->asObject()->id());
		resp.set_drm_connector_type(self->_device->connectorType());
		resp.set_drm_connector_type_id(0);
		resp.set_drm_connection(conn->getCurrentStatus()); // DRM_MODE_CONNECTED
		resp.set_drm_mm_width(self->_device->getPhysicalWidth());
		resp.set_drm_mm_height(self->_device->getPhysicalHeight());
		resp.set_drm_subpixel(self->_device->getSubpixel());
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
		resp.set_drm_crtc_id(enc->currentCrtc()->asObject()->id());
		
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
		resp.set_drm_fb_id(fb->asObject()->id());
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
	
		std::vector<drm_backend::Assignment> assignments;
		if(req.drm_mode_valid()) {
			auto mode_blob = std::make_shared<Blob>(std::move(mode_buffer));
			assignments.push_back(Assignment{ 
				crtc->asObject(),
				&self->_device->modeIdProperty,
				0,
				nullptr,
				mode_blob
			});
			
			auto id = req.drm_fb_id();
			auto fb = self->_device->findObject(id);
			assert(fb);
			assignments.push_back(Assignment{ 
				crtc->primaryPlane()->asObject(),
				&self->_device->fbIdProperty, 
				0,
				fb,
				nullptr
			});
		}else{
			std::vector<drm_backend::Assignment> assignments;
			assignments.push_back(Assignment{ 
				crtc->asObject(),
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

constexpr auto fileOperations = protocols::fs::FileOperations{}
	.withRead(&drm_backend::File::read)
	.withAccessMemory(&drm_backend::File::accessMemory)
	.withIoctl(&drm_backend::File::ioctl);

COFIBER_ROUTINE(cofiber::no_future, serveDevice(std::shared_ptr<drm_backend::Device> device,
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
			auto file = std::make_shared<drm_backend::File>(device);
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
	_theEncoder = std::make_shared<Encoder>(this);
	_theConnector = std::make_shared<Connector>(this);
	_primaryPlane = std::make_shared<Plane>(this);
	
	registerObject(_theCrtc);
	registerObject(_theEncoder);
	registerObject(_theConnector);
	registerObject(_primaryPlane);
	
	_theEncoder->setCurrentCrtc(_theCrtc.get());
	_theConnector->setCurrentEncoder(_theEncoder.get());
	_theConnector->setCurrentStatus(1);
	_theEncoder->setupPossibleCrtcs({_theCrtc.get()});
	_theEncoder->setupPossibleClones({_theEncoder.get()});

	setupCrtc(_theCrtc);
	setupEncoder(_theEncoder);
	attachConnector(_theConnector);

	drm_mode_modeinfo mode;
	mode.clock = 47185;
	mode.hdisplay = 1024;
	mode.hsync_start = 1024;
	mode.hsync_end = 1024;
	mode.htotal = 1024;
	mode.hskew = 0;
	mode.vdisplay = 768;
	mode.vsync_start = 768;
	mode.vsync_end = 768;
	mode.vtotal = 768;
	mode.vscan = 0;
	mode.vrefresh = 60;
	mode.flags = 0;
	mode.type = 0;
	memcpy(&mode.name, "1024x768", 8);
	std::vector<drm_mode_modeinfo> mode_list;
	mode_list.push_back(mode);
	_theConnector->setModeList(mode_list);
	
	setupMinDimensions(640, 480);
	setupMaxDimensions(1024, 768);
		
	setupPhysicalDimensions(306, 230);
	setupSubpixel(0);
}))
	
std::unique_ptr<drm_backend::Configuration> GfxDevice::createConfiguration() {
	return std::make_unique<Configuration>(this);
}

std::shared_ptr<drm_backend::FrameBuffer> GfxDevice::createFrameBuffer(std::shared_ptr<drm_backend::BufferObject> base_bo,
		uint32_t width, uint32_t height, uint32_t format, uint32_t pitch) {
	auto bo = std::static_pointer_cast<GfxDevice::BufferObject>(base_bo);
		
	assert(pitch % 4 == 0);
	auto pixel_pitch = pitch / 4;
	
	assert(pixel_pitch >= width);
	assert(bo->getAddress() % pitch == 0);
	assert(bo->getSize() >= pitch * height);

	auto fb = std::make_shared<FrameBuffer>(this, bo, pixel_pitch);
	registerObject(fb);
	return fb;
}

std::pair<std::shared_ptr<drm_backend::BufferObject>, uint32_t> GfxDevice::createDumb(uint32_t width,
		uint32_t height, uint32_t bpp) {
	assert(width <= 1024);
	auto size = height * 4096;
	auto address = _vramAllocator.allocate(size);

	auto buffer = std::make_shared<BufferObject>(this, address, size);
	auto mapping = installMapping(buffer.get());
	buffer->setupMapping(mapping);
	return std::make_pair(buffer, 4096);
}

// ----------------------------------------------------------------
// GfxDevice::Configuration.
// ----------------------------------------------------------------

bool GfxDevice::Configuration::capture(std::vector<drm_backend::Assignment> assignment) {
	for(auto &assign: assignment) {
		if(assign.property == &_device->srcWProperty) {
			_width = assign.intValue;
		}else if(assign.property == &_device->srcHProperty) {
			_height = assign.intValue;
		}else if(assign.property == &_device->fbIdProperty) {
			auto fb = assign.objectValue->asFrameBuffer();
			if(!fb)
				return false;
			_fb = static_cast<GfxDevice::FrameBuffer *>(fb);
		}else if(assign.property == &_device->modeIdProperty) {
			drm_mode_modeinfo mode_info;
			_mode = assign.blobValue; 
			if(_mode) {
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
	_device->_theCrtc->setCurrentMode(_mode);

	if(_mode) {
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::enable);
		_device->_operational.store(regs::data, enable_bits::noMemClear | enable_bits::lfb);
	
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::virtWidth);
		_device->_operational.store(regs::data, _fb->getPixelPitch());
		
		/* You don't need to initialize this register.
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::virtHeight);
		_device->_operational.store(regs::data, _height);
		*/
		
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::bpp);
		_device->_operational.store(regs::data, 32);
	
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::resX);
		_device->_operational.store(regs::data, _width);
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::resY);
		_device->_operational.store(regs::data, _height);
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::offX);
		_device->_operational.store(regs::data, 0);
		
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::offY);
		_device->_operational.store(regs::data, _fb->getBufferObject()->getAddress() / (_fb->getPixelPitch() * 4));

		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::enable);
		_device->_operational.store(regs::data, enable_bits::enable 
				| enable_bits::noMemClear | enable_bits::lfb);
	}else{
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::enable);
		_device->_operational.store(regs::data, enable_bits::noMemClear | enable_bits::lfb);
	}
}

// ----------------------------------------------------------------
// GfxDevice::Connector.
// ----------------------------------------------------------------

GfxDevice::Connector::Connector(GfxDevice *device)
	: drm_backend::Object { device->allocator.allocate() } {
	_encoders.push_back(device->_theEncoder.get());
}

drm_backend::Connector *GfxDevice::Connector::asConnector() {
	return this;
}

drm_backend::Object *GfxDevice::Connector::asObject() {
	return this;
}
		
const std::vector<drm_backend::Encoder *> &GfxDevice::Connector::possibleEncoders() {
	return _encoders;
}

// ----------------------------------------------------------------
// GfxDevice::Encoder.
// ----------------------------------------------------------------

GfxDevice::Encoder::Encoder(GfxDevice *device)
	:drm_backend::Object { device->allocator.allocate() } {
}

drm_backend::Encoder *GfxDevice::Encoder::asEncoder() {
	return this;
}

drm_backend::Object *GfxDevice::Encoder::asObject() {
	return this;
}

// ----------------------------------------------------------------
// GfxDevice::Crtc.
// ----------------------------------------------------------------

GfxDevice::Crtc::Crtc(GfxDevice *device)
	:drm_backend::Object { device->allocator.allocate() } {
	_device = device;
}

drm_backend::Crtc *GfxDevice::Crtc::asCrtc() {
	return this;
}

drm_backend::Object *GfxDevice::Crtc::asObject() {
	return this;
}

drm_backend::Plane *GfxDevice::Crtc::primaryPlane() {
	return _device->_primaryPlane.get(); 
}

// ----------------------------------------------------------------
// GfxDevice::FrameBuffer.
// ----------------------------------------------------------------

GfxDevice::FrameBuffer::FrameBuffer(GfxDevice *device, std::shared_ptr<GfxDevice::BufferObject> bo,
		uint32_t pixel_pitch)
	:drm_backend::Object { device->allocator.allocate() } {
	_bo = bo;
	_pixelPitch = pixel_pitch;
}

drm_backend::FrameBuffer *GfxDevice::FrameBuffer::asFrameBuffer() {
	return this;
}

drm_backend::Object *GfxDevice::FrameBuffer::asObject() {
	return this;
}

GfxDevice::BufferObject *GfxDevice::FrameBuffer::getBufferObject() {
	return _bo.get();
}

uint32_t GfxDevice::FrameBuffer::getPixelPitch() {
	return _pixelPitch;
}

// ----------------------------------------------------------------
// GfxDevice: Plane.
// ----------------------------------------------------------------

GfxDevice::Plane::Plane(GfxDevice *device)
	:drm_backend::Object { device->allocator.allocate() } {
}

drm_backend::Plane *GfxDevice::Plane::asPlane() {
	return this;
}

drm_backend::Object *GfxDevice::Plane::asObject() {
	return this;
}

// ----------------------------------------------------------------
// GfxDevice: BufferObject.
// ----------------------------------------------------------------

std::shared_ptr<drm_backend::BufferObject> GfxDevice::BufferObject::sharedBufferObject() {
	return this->shared_from_this();
}
		
uintptr_t GfxDevice::BufferObject::getAddress() {
	return _address;
}

size_t GfxDevice::BufferObject::getSize() {
	return _size;
}
	
std::pair<helix::BorrowedDescriptor, uint64_t> GfxDevice::BufferObject::getMemory() {
	return std::make_pair(helix::BorrowedDescriptor(_device->_videoRam), _address);
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
		{"unix.devname", mbus::StringItem{"card0"}}
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

