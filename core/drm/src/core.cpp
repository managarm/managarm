
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
		: drm_core::Property{srcW, drm_core::IntPropertyType{}, "SRC_W"} { }

		bool validate(const Assignment&) override {
			return true;
		};

		void writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->plane(assignment.object->id())->src_w(assignment.intValue >> 16);
		}

		uint32_t intFromState(std::shared_ptr<ModeObject> obj) override {
			auto plane = obj->asPlane();
			assert(plane);
			return plane->drmState()->src_w();
		}
	};
	registerProperty(_srcWProperty = std::make_shared<SrcWProperty>());

	struct SrcHProperty : drm_core::Property {
		SrcHProperty()
		: drm_core::Property{srcH, drm_core::IntPropertyType{}, "SRC_H"} { }

		bool validate(const Assignment&) override {
			return true;
		};

		void writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->plane(assignment.object->id())->src_h(assignment.intValue >> 16);
		}

		uint32_t intFromState(std::shared_ptr<ModeObject> obj) override {
			auto plane = obj->asPlane();
			assert(plane);
			return plane->drmState()->src_h();
		}
	};
	registerProperty(_srcHProperty = std::make_shared<SrcHProperty>());

	struct FbIdProperty : drm_core::Property {
		FbIdProperty()
		: drm_core::Property{fbId, drm_core::ObjectPropertyType{}, "FB_ID"} { }

		bool validate(const Assignment& assignment) override {
			if(!assignment.objectValue)
				return true;

			if(assignment.objectValue->asFrameBuffer())
				return true;

			return false;
		};

		void writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->plane(assignment.object->id())->fb_id(static_pointer_cast<FrameBuffer>(assignment.objectValue));
		}

		std::shared_ptr<ModeObject> modeObjFromState(std::shared_ptr<ModeObject> obj) override {
			auto plane = obj->asPlane();
			assert(plane);
			return static_pointer_cast<ModeObject>(plane->drmState()->fb_id());
		}
	};
	registerProperty(_fbIdProperty = std::make_shared<FbIdProperty>());

	struct ModeIdProperty : drm_core::Property {
		ModeIdProperty()
		: drm_core::Property{modeId, drm_core::BlobPropertyType{}, "MODE_ID"} { }

		bool validate(const Assignment& assignment) override {
			if(!assignment.blobValue) {
				return true;
			}

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

		void writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->crtc(assignment.object->id())->mode(assignment.blobValue);
		}
	};
	registerProperty(_modeIdProperty = std::make_shared<ModeIdProperty>());

	struct CrtcXProperty : drm_core::Property {
		CrtcXProperty()
		: drm_core::Property{crtcX, drm_core::IntPropertyType{}, "CRTC_X"} { }

		bool validate(const Assignment&) override {
			return true;
		};
	};
	registerProperty(_crtcXProperty = std::make_shared<CrtcXProperty>());

	struct CrtcYProperty : drm_core::Property {
		CrtcYProperty()
		: drm_core::Property{crtcY, drm_core::IntPropertyType{}, "CRTC_Y"} { }

		bool validate(const Assignment&) override {
			return true;
		};
	};
	registerProperty(_crtcYProperty = std::make_shared<CrtcYProperty>());

	struct PlaneTypeProperty : drm_core::Property {
		PlaneTypeProperty()
		: drm_core::Property{planeType, drm_core::EnumPropertyType{}, "type"} {
			addEnumInfo(0, "Overlay");
			addEnumInfo(1, "Primary");
			addEnumInfo(2, "Cursor");
		}

		bool validate(const Assignment& assignment) override {
			auto plane = assignment.object->asPlane();
			assert(plane);
			return (static_cast<uint64_t>(plane->type()) == assignment.intValue);
		}
	};
	registerProperty(_planeTypeProperty = std::make_shared<PlaneTypeProperty>());

	struct DpmsProperty : drm_core::Property {
		DpmsProperty()
		: drm_core::Property{dpms, drm_core::IntPropertyType{}, "DPMS"} { }

		bool validate(const Assignment& assignment) override {
			return (assignment.intValue < 4);
		}

		void writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->connector(assignment.object->id())->dpms(assignment.intValue);
		}
	};
	registerProperty(_dpmsProperty = std::make_shared<DpmsProperty>());

	struct CrtcIdProperty : drm_core::Property {
		CrtcIdProperty()
		: drm_core::Property{crtcId, drm_core::ObjectPropertyType{}, "CRTC_ID"} { }

		bool validate(const Assignment&) override {
			return true;
		};
	};
	registerProperty(_crtcIdProperty = std::make_shared<CrtcIdProperty>());

	struct ActiveProperty : drm_core::Property {
		ActiveProperty()
		: drm_core::Property{active, drm_core::IntPropertyType{}, "ACTIVE"} { }

		bool validate(const Assignment&) override {
			return true;
		};
	};
	registerProperty(_activeProperty = std::make_shared<ActiveProperty>());

	struct SrcXProperty : drm_core::Property {
		SrcXProperty()
		: drm_core::Property{srcX, drm_core::IntPropertyType{}, "SRC_X"} { }

		bool validate(const Assignment&) override {
			return true;
		};

		void writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->plane(assignment.object->id())->src_x(assignment.intValue);
		}
	};
	registerProperty(_srcXProperty = std::make_shared<SrcXProperty>());

	struct SrcYProperty : drm_core::Property {
		SrcYProperty()
		: drm_core::Property{srcY, drm_core::IntPropertyType{}, "SRC_Y"} { }

		bool validate(const Assignment&) override {
			return true;
		};

		void writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->plane(assignment.object->id())->src_y(assignment.intValue);
		}
	};
	registerProperty(_srcYProperty = std::make_shared<SrcYProperty>());

	struct CrtcWProperty : drm_core::Property {
		CrtcWProperty()
		: drm_core::Property{crtcW, drm_core::IntPropertyType{}, "CRTC_W"} { }

		bool validate(const Assignment&) override {
			return true;
		};
	};
	registerProperty(_crtcWProperty = std::make_shared<CrtcWProperty>());

	struct CrtcHProperty : drm_core::Property {
		CrtcHProperty()
		: drm_core::Property{crtcH, drm_core::IntPropertyType{}, "CRTC_H"} { }

		bool validate(const Assignment&) override {
			return true;
		};
	};
	registerProperty(_crtcHProperty = std::make_shared<CrtcHProperty>());
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

uint32_t drm_core::Device::registerBlob(std::shared_ptr<drm_core::Blob> blob) {
	uint32_t id = _blobIdAllocator.allocate();
	_blobs.insert({id, blob});

	return id;
}

bool drm_core::Device::deleteBlob(uint32_t id) {
	if(_blobs.contains(id)) {
		_blobs.erase(id);
		return true;
	}

	return false;
}

std::shared_ptr<drm_core::Blob> drm_core::Device::findBlob(uint32_t id) {
	auto it = _blobs.find(id);
	if(it == _blobs.end())
		return nullptr;
	return it->second;
}

std::unique_ptr<drm_core::AtomicState> drm_core::Device::atomicState() {
	auto state = AtomicState(this);
	return std::make_unique<drm_core::AtomicState>(state);
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

drm_core::Property *drm_core::Device::planeTypeProperty() {
	return _planeTypeProperty.get();
}

drm_core::Property *drm_core::Device::dpmsProperty() {
	return _dpmsProperty.get();
}

drm_core::Property *drm_core::Device::crtcIdProperty() {
	return _crtcIdProperty.get();
}

drm_core::Property *drm_core::Device::activeProperty() {
	return _activeProperty.get();
}

drm_core::Property *drm_core::Device::srcXProperty() {
	return _srcXProperty.get();
}

drm_core::Property *drm_core::Device::srcYProperty() {
	return _srcYProperty.get();
}

drm_core::Property *drm_core::Device::crtcWProperty() {
	return _crtcWProperty.get();
}

drm_core::Property *drm_core::Device::crtcHProperty() {
	return _crtcHProperty.get();
}

void drm_core::Device::registerProperty(std::shared_ptr<drm_core::Property> p) {
	_properties.insert({p->id(), p});
}

std::shared_ptr<drm_core::Property> drm_core::Device::getProperty(uint32_t id) {
	auto it = _properties.find(id);

	if(it == _properties.end()) {
		return nullptr;
	}

	return it->second;
}

// ----------------------------------------------------------------
// Property
// ----------------------------------------------------------------

std::vector<drm_core::Assignment> drm_core::ModeObject::getAssignments(std::shared_ptr<Device> dev) {
	switch(this->type()) {
		case ObjectType::connector: {
			auto connector = this->asConnector();
			assert(connector);
			return connector->getAssignments(dev);
		}
		case ObjectType::crtc: {
			auto crtc = this->asCrtc();
			assert(crtc);
			return crtc->getAssignments(dev);
		}
		case ObjectType::plane: {
			auto plane = this->asPlane();
			assert(plane);
			return plane->getAssignments(dev);
		}
		default: {
			std::cout << "core/drm: ModeObj " << this->id() << " doesn't support querying DRM properties (yet)" << std::endl;
			break;
		}
	}

	std::vector<drm_core::Assignment> assignments = std::vector<drm_core::Assignment>();

	return assignments;
}

bool drm_core::Property::validate(const Assignment&) {
	return true;
}

drm_core::PropertyId drm_core::Property::id() {
	return _id;
}

uint32_t drm_core::Property::flags() {
	return _flags;
}

drm_core::PropertyType drm_core::Property::propertyType() {
	return _propertyType;
}

std::string drm_core::Property::name() {
	return _name;
}

void drm_core::Property::addEnumInfo(uint64_t value, std::string name) {
	assert(std::holds_alternative<EnumPropertyType>(_propertyType));
	_enum_info.insert({value, name});
}

const std::unordered_map<uint64_t, std::string>& drm_core::Property::enumInfo() {
	return _enum_info;
}

void drm_core::Property::writeToState(const drm_core::Assignment assignment, std::unique_ptr<drm_core::AtomicState> &state) {
	(void) assignment;
	(void) state;
	return;
}

uint32_t drm_core::Property::intFromState(std::shared_ptr<drm_core::ModeObject> obj) {
	(void) obj;
	return 0;
}

std::shared_ptr<drm_core::ModeObject> drm_core::Property::modeObjFromState(std::shared_ptr<drm_core::ModeObject> obj) {
	(void) obj;
	return nullptr;
}

// ----------------------------------------------------------------
// Assignment
// ----------------------------------------------------------------

drm_core::Assignment drm_core::Assignment::withInt(std::shared_ptr<drm_core::ModeObject> obj, drm_core::Property *property, uint64_t val) {
	return drm_core::Assignment{obj, property, val, nullptr, nullptr};
}

drm_core::Assignment drm_core::Assignment::withModeObj(std::shared_ptr<drm_core::ModeObject> obj, drm_core::Property *property, std::shared_ptr<drm_core::ModeObject> modeobj) {
	return drm_core::Assignment{obj, property, 0, modeobj, nullptr};
}

drm_core::Assignment drm_core::Assignment::withBlob(std::shared_ptr<drm_core::ModeObject> obj, drm_core::Property *property, std::shared_ptr<drm_core::Blob> blob) {
	return drm_core::Assignment{obj, property, 0, nullptr, blob};
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

drm_core::ObjectType drm_core::ModeObject::type() {
	return _type;
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

void drm_core::Crtc::setupState(std::shared_ptr<drm_core::Crtc> crtc) {
	crtc->_drmState = std::make_shared<drm_core::CrtcState>(drm_core::CrtcState(crtc));
}

drm_core::Plane *drm_core::Crtc::cursorPlane() {
	return nullptr;
}

std::shared_ptr<drm_core::CrtcState> drm_core::Crtc::drmState() {
	return _drmState;
}

void drm_core::Crtc::setDrmState(std::shared_ptr<drm_core::CrtcState> new_state) {
	_drmState = new_state;
}

std::vector<drm_core::Assignment> drm_core::Crtc::getAssignments(std::shared_ptr<drm_core::Device> dev) {
	std::vector<drm_core::Assignment> assignments = std::vector<drm_core::Assignment>();

	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->activeProperty(), drmState()->active()));
	assignments.push_back(drm_core::Assignment::withBlob(this->sharedModeObject(), dev->modeIdProperty(), drmState()->mode()));

	return assignments;
}

drm_core::CrtcState::CrtcState(std::weak_ptr<Crtc> crtc)
	: _crtc(crtc) {

}

std::weak_ptr<drm_core::Crtc> drm_core::CrtcState::crtc(void) {
	return _crtc;
}

std::shared_ptr<drm_core::Blob> drm_core::CrtcState::mode() {
	return _mode;
}

void drm_core::CrtcState::mode(std::shared_ptr<drm_core::Blob> mode) {
	_mode = mode;
	_modeChanged = true;
}

bool drm_core::CrtcState::mode_changed() {
	return _modeChanged;
}

bool drm_core::CrtcState::active() {
	return _active;
}

void drm_core::CrtcState::active(bool active) {
	_active = active;
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

drm_core::Plane::Plane(uint32_t id, PlaneType type)
	:drm_core::ModeObject { ObjectType::plane, id }, _type(type) {
}

void drm_core::Plane::setupState(std::shared_ptr<drm_core::Plane> plane) {
	plane->_drmState = std::make_shared<drm_core::PlaneState>(drm_core::PlaneState(plane));
}

drm_core::Plane::PlaneType drm_core::Plane::type() {
	return _type;
}

void drm_core::Plane::setCurrentFrameBuffer(drm_core::FrameBuffer *fb) {
	_fb = fb;
}

drm_core::FrameBuffer *drm_core::Plane::getFrameBuffer() {
	return _fb;
}

void drm_core::Plane::setupPossibleCrtcs(std::vector<drm_core::Crtc *> crtcs) {
	_possibleCrtcs = crtcs;
}

const std::vector<drm_core::Crtc *> &drm_core::Plane::getPossibleCrtcs() {
	return _possibleCrtcs;
}

std::shared_ptr<drm_core::PlaneState> drm_core::Plane::drmState() {
	return _drmState;
}

void drm_core::Plane::setDrmState(std::shared_ptr<drm_core::PlaneState> new_state) {
	_drmState = new_state;
}

std::vector<drm_core::Assignment> drm_core::Plane::getAssignments(std::shared_ptr<drm_core::Device> dev) {
	std::vector<drm_core::Assignment> assignments = std::vector<drm_core::Assignment>();

	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->planeTypeProperty(), static_cast<uint64_t>(drmState()->type())));
	assignments.push_back(drm_core::Assignment::withModeObj(this->sharedModeObject(), dev->crtcIdProperty(), drmState()->crtc()));
	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->srcHProperty(), drmState()->src_h()));
	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->srcWProperty(), drmState()->src_w()));
	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->crtcHProperty(), drmState()->crtc_h()));
	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->crtcWProperty(), drmState()->crtc_w()));
	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->srcXProperty(), drmState()->src_x()));
	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->srcYProperty(), drmState()->src_y()));
	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->crtcXProperty(), drmState()->crtc_x()));
	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->crtcYProperty(), drmState()->crtc_y()));
	assignments.push_back(drm_core::Assignment::withModeObj(this->sharedModeObject(), dev->fbIdProperty(), drmState()->fb_id()));

	return assignments;
}

std::shared_ptr<drm_core::Plane> drm_core::PlaneState::plane(void) {
	return _plane;
}

drm_core::Plane::PlaneType drm_core::PlaneState::type(void) {
	return _plane->type();
}

void drm_core::PlaneState::src_w(uint32_t value) {
	_srcW = value;
}

uint32_t drm_core::PlaneState::src_w() {
	return _srcW;
}

void drm_core::PlaneState::src_h(uint32_t value) {
	_srcH = value;
}

uint32_t drm_core::PlaneState::src_h() {
	return _srcH;
}

void drm_core::PlaneState::src_x(uint32_t value) {
	_srcX = value;
}

uint32_t drm_core::PlaneState::src_x() {
	return _srcX;
}

void drm_core::PlaneState::src_y(uint32_t value) {
	_srcY = value;
}

uint32_t drm_core::PlaneState::src_y() {
	return _srcY;
}

void drm_core::PlaneState::crtc_w(uint32_t value) {
	_crtcW = value;
}

uint32_t drm_core::PlaneState::crtc_w() {
	return _crtcW;
}

void drm_core::PlaneState::crtc_h(uint32_t value) {
	_crtcH = value;
}

uint32_t drm_core::PlaneState::crtc_h() {
	return _crtcH;
}

void drm_core::PlaneState::crtc_x(uint32_t value) {
	_crtcX = value;
}

uint32_t drm_core::PlaneState::crtc_x() {
	return _crtcX;
}

void drm_core::PlaneState::crtc_y(uint32_t value) {
	_crtcY = value;
}

uint32_t drm_core::PlaneState::crtc_y() {
	return _crtcY;
}

void drm_core::PlaneState::crtc(std::shared_ptr<drm_core::Crtc> value) {
	_crtc = value;
}

std::shared_ptr<drm_core::Crtc> drm_core::PlaneState::crtc() {
	return _crtc;
}

void drm_core::PlaneState::fb_id(std::shared_ptr<drm_core::FrameBuffer> value) {
	_fb = value;
}

std::shared_ptr<drm_core::FrameBuffer> drm_core::PlaneState::fb_id() {
	return _fb;
}

// ----------------------------------------------------------------
// Connector
// ----------------------------------------------------------------

drm_core::Connector::Connector(uint32_t id)
	:drm_core::ModeObject { ObjectType::connector, id } {
	_currentEncoder = nullptr;
	_connectorType = 0;
}

void drm_core::Connector::setupState(std::shared_ptr<drm_core::Connector> connector) {
	connector->_drmState = std::make_shared<drm_core::ConnectorState>(drm_core::ConnectorState(connector));
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

void drm_core::Connector::setConnectorType(uint32_t type) {
	_connectorType = type;
}

uint32_t drm_core::Connector::connectorType() {
	return _connectorType;
}

std::shared_ptr<drm_core::ConnectorState> drm_core::Connector::drmState() {
	return _drmState;
}

void drm_core::Connector::setDrmState(std::shared_ptr<drm_core::ConnectorState> new_state) {
	_drmState = new_state;
}

std::vector<drm_core::Assignment> drm_core::Connector::getAssignments(std::shared_ptr<drm_core::Device> dev) {
	std::vector<drm_core::Assignment> assignments = std::vector<drm_core::Assignment>();

	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->dpmsProperty(), drmState()->dpms()));
	assignments.push_back(drm_core::Assignment::withModeObj(this->sharedModeObject(), dev->crtcIdProperty(), drmState()->crtc()));

	return assignments;
}

std::shared_ptr<drm_core::Crtc> drm_core::ConnectorState::crtc(void) {
	return _crtc;
}

void drm_core::ConnectorState::crtc(std::shared_ptr<drm_core::Crtc> val) {
	_crtc = val;
}

uint32_t drm_core::ConnectorState::dpms(void) {
	return _dpms;
}

void drm_core::ConnectorState::dpms(uint32_t val) {
	_dpms = val;
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

std::shared_ptr<drm_core::CrtcState> drm_core::AtomicState::crtc(uint32_t id) {
	if(_crtcStates.contains(id)) {
		return _crtcStates.find(id)->second;
	} else {
		auto crtc = _device->findObject(id)->asCrtc();
		assert(crtc->drmState());
		auto crtc_state = CrtcState(*crtc->drmState());
		auto crtc_state_shared = std::make_shared<drm_core::CrtcState>(crtc_state);
		_crtcStates.insert({id, crtc_state_shared});
		return crtc_state_shared;
	}
}

std::shared_ptr<drm_core::PlaneState> drm_core::AtomicState::plane(uint32_t id) {
	if(_planeStates.contains(id)) {
		return _planeStates.find(id)->second;
	} else {
		auto plane = _device->findObject(id)->asPlane();
		assert(plane->drmState());
		auto plane_state = PlaneState(*plane->drmState());
		auto plane_state_shared = std::make_shared<drm_core::PlaneState>(plane_state);
		_planeStates.insert({id, plane_state_shared});
		return plane_state_shared;
	}
}

std::shared_ptr<drm_core::ConnectorState> drm_core::AtomicState::connector(uint32_t id) {
	if(_connectorStates.contains(id)) {
		return _connectorStates.find(id)->second;
	} else {
		auto connector = _device->findObject(id)->asConnector();
		assert(connector->drmState());
		auto connector_state = ConnectorState(*connector->drmState());
		auto connector_state_shared = std::make_shared<drm_core::ConnectorState>(connector_state);
		_connectorStates.insert({id, connector_state_shared});
		return connector_state_shared;
	}
}

std::unordered_map<uint32_t, std::shared_ptr<drm_core::CrtcState>>& drm_core::AtomicState::crtc_states(void) {
	return _crtcStates;
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
	out.crtc_id = ev->crtcId;
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

		resp.set_error(managarm::fs::Errors::SUCCESS);

		if(req.drm_capability() == DRM_CAP_TIMESTAMP_MONOTONIC) {
			resp.set_drm_value(1);
		}else if(req.drm_capability() == DRM_CAP_DUMB_BUFFER) {
			resp.set_drm_value(1);
		}else if(req.drm_capability() == DRM_CAP_CRTC_IN_VBLANK_EVENT) {
			resp.set_drm_value(1);
		}else if(req.drm_capability() == DRM_CAP_CURSOR_WIDTH) {
			resp.set_drm_value(0);
		}else if(req.drm_capability() == DRM_CAP_CURSOR_HEIGHT) {
			resp.set_drm_value(0);
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
		for(size_t i = 0; i < crtcs.size(); i++) {
			resp.add_drm_crtc_ids(crtcs[i]->id());
		}

		auto &encoders = self->_device->getEncoders();
		for(size_t i = 0; i < encoders.size(); i++) {
			resp.add_drm_encoder_ids(encoders[i]->id());
		}

		auto &connectors = self->_device->getConnectors();
		for(size_t i = 0; i < connectors.size(); i++) {
			resp.add_drm_connector_ids(connectors[i]->id());
		}

		auto &fbs = self->getFrameBuffers();
		for(size_t i = 0; i < fbs.size(); i++) {
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
		for(size_t i = 0; i < psbl_enc.size(); i++) {
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
	}else if(req.command() == DRM_IOCTL_MODE_GETPLANE) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		resp.set_drm_encoder_type(0);

		auto obj = self->_device->findObject(req.drm_plane_id());
		assert(obj);
		auto plane = obj->asPlane();
		assert(plane);

		uint32_t crtc_mask = 0;
		for(auto crtc : plane->getPossibleCrtcs()) {
			crtc_mask |= 1 << crtc->index;
		}
		resp.set_drm_possible_crtcs(crtc_mask);

		auto crtc = plane->drmState()->crtc();
		if(crtc != nullptr) {
			resp.set_drm_crtc_id(crtc->id());
		} else {
			resp.set_drm_crtc_id(0);
		}

		auto fb = plane->getFrameBuffer();
		if(fb) {
			resp.set_drm_fb_id(fb->id());
		} else {
			resp.set_drm_fb_id(0);
		}

		resp.set_drm_gamma_size(0);
		resp.add_drm_format_type(DRM_FORMAT_XRGB8888);

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
		if(crtc->drmState()->mode()) {
			/* TODO: Set x, y, fb_id, gamma_size */
			memcpy(&mode_info, crtc->drmState()->mode()->data(), sizeof(drm_mode_modeinfo));
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
			auto fb = self->_device->findObject(req.drm_fb_id());
			assert(fb);

			assignments.push_back(Assignment::withBlob(crtc->sharedModeObject(), self->_device->modeIdProperty(), mode_blob));
			assignments.push_back(Assignment::withModeObj(crtc->primaryPlane()->sharedModeObject(), self->_device->fbIdProperty(), fb));
		}else{
			assignments.push_back(Assignment::withBlob(crtc->sharedModeObject(), self->_device->modeIdProperty(), nullptr));
		}

		auto config = self->_device->createConfiguration();
		auto state = self->_device->atomicState();
		auto valid = config->capture(assignments, state);
		assert(valid);
		config->commit(state);

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
		assignments.push_back(Assignment::withModeObj(crtc->primaryPlane()->sharedModeObject(), self->_device->fbIdProperty(), fb));

		auto config = self->_device->createConfiguration();
		auto state = self->_device->atomicState();
		auto valid = config->capture(assignments, state);
		assert(valid);
		config->commit(state);

		self->_retirePageFlip(std::move(config), req.drm_cookie(), crtc->id());

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

			assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->srcWProperty(), width << 16));
			assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->srcHProperty(), height << 16));

			if (bo) {
				auto fb = self->_device->createFrameBuffer(bo->sharedBufferObject(), width, height, DRM_FORMAT_ARGB8888, width * 4);
				assert(fb);
				assignments.push_back(Assignment::withModeObj(crtc->cursorPlane()->sharedModeObject(), self->_device->fbIdProperty(), fb));
			} else {
				assignments.push_back(Assignment::withModeObj(crtc->cursorPlane()->sharedModeObject(), self->_device->fbIdProperty(), nullptr));
			}
		}else if (req.drm_flags() == DRM_MODE_CURSOR_MOVE) {
			resp.set_error(managarm::fs::Errors::SUCCESS);
			auto x = req.drm_x();
			auto y = req.drm_y();

			assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->crtcXProperty(), x));
			assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->crtcYProperty(), y));
		}else{
			printf("\e[35mcore/drm: invalid request whilst handling DRM_IOCTL_MODE_CURSOR\e[39m\n");
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}

		auto config = self->_device->createConfiguration();
		auto state = self->_device->atomicState();
		auto valid = config->capture(assignments, state);
		assert(valid);
		config->commit(state);

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
	}else if(req.command() == DRM_IOCTL_SET_CLIENT_CAP) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		if(req.drm_capability() == DRM_CLIENT_CAP_STEREO_3D) {
			std::cout << "\e[31mcore/drm: DRM client cap for stereo 3D unsupported\e[39m" << std::endl;
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		} else if(req.drm_capability() == DRM_CLIENT_CAP_UNIVERSAL_PLANES) {
			self->universalPlanes = true;
			resp.set_error(managarm::fs::Errors::SUCCESS);
		} else if(req.drm_capability() == DRM_CLIENT_CAP_ATOMIC) {
			self->atomic = true;
			self->universalPlanes = true;
			resp.set_error(managarm::fs::Errors::SUCCESS);
		} else {
			std::cout << "\e[31mcore/drm: Attempt to set unknown client capability " << req.drm_capability() << "\e[39m" << std::endl;
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_OBJ_GETPROPERTIES) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto obj = self->_device->findObject(req.drm_obj_id());
		assert(obj);

		for(auto ass : obj->getAssignments(self->_device)) {
			resp.add_drm_obj_property_ids(ass.property->id());
			resp.add_drm_obj_property_values(ass.intValue);
		}

		if(!resp.drm_obj_property_ids_size()) {
			std::cout << "\e[31mcore/drm: No properties found for object id " << req.drm_obj_id() << "\e[39m" << std::endl;
		}

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETPROPERTY) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		uint32_t prop_id = req.drm_property_id();
		auto prop = self->_device->getProperty(prop_id);

		if(prop) {
			resp.set_drm_property_name(prop->name());
			resp.add_drm_property_vals(1);
			resp.set_drm_property_flags(prop->flags());

			if(std::holds_alternative<EnumPropertyType>(prop->propertyType())) {
				for(auto &[value, name] : prop->enumInfo()) {
					resp.add_drm_enum_value(value);
					resp.add_drm_enum_name(name);
				}
			}

			resp.set_error(managarm::fs::Errors::SUCCESS);
		} else {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_SETPROPERTY) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		std::cout << "\e[31mcore/drm: DRM_IOCTL_MODE_SETPROPERTY is a no-op\e[39m"
				<< std::endl;

		resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETPLANERESOURCES) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto &crtcs = self->_device->getCrtcs();
		for(size_t i = 0; i < crtcs.size(); i++) {
			resp.add_drm_plane_res(crtcs[i]->primaryPlane()->id());

			if(crtcs[i]->cursorPlane()) {
				resp.add_drm_plane_res(crtcs[i]->cursorPlane()->id());
			}
		}

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETPROPBLOB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto blob = self->_device->findBlob(req.drm_blob_id());

		if(blob) {
			auto data = reinterpret_cast<const uint8_t *>(blob->data());
			for(size_t i = 0; i < blob->size(); i++) {
				resp.add_drm_property_blob(data[i]);
			}

			resp.set_error(managarm::fs::Errors::SUCCESS);
		} else {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_CREATEPROPBLOB) {
		std::vector<char> blob_data;
		blob_data.resize(req.drm_blob_size());

		helix::RecvBuffer recv_buffer;
		auto &&buff = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&recv_buffer, blob_data.data(), req.drm_blob_size()));
		co_await buff.async_wait();
		HEL_CHECK(recv_buffer.error());

		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		if(!req.drm_blob_size()) {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		} else {
			auto blob = std::make_shared<Blob>(std::move(blob_data));
			auto id = self->_device->registerBlob(blob);

			resp.set_drm_blob_id(id);
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_DESTROYPROPBLOB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		if(!self->_device->deleteBlob(req.drm_blob_id())) {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		} else {
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_ATOMIC) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		size_t prop_count = 0;
		std::vector<drm_core::Assignment> assignments;

		std::vector<uint32_t> crtc_ids;

		auto config = self->_device->createConfiguration();
		auto state = self->_device->atomicState();

		if(!self->atomic || req.drm_flags() & ~DRM_MODE_ATOMIC_FLAGS || ((req.drm_flags() & DRM_MODE_ATOMIC_TEST_ONLY) && (req.drm_flags() & DRM_MODE_PAGE_FLIP_EVENT))) {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			goto send;
		}

		for(size_t i = 0; i < req.drm_obj_ids_size(); i++) {
			auto mode_obj = self->_device->findObject(req.drm_obj_ids(i));
			assert(mode_obj);

			if(mode_obj->type() == ObjectType::crtc) {
				crtc_ids.push_back(mode_obj->id());
			}

			for(size_t j = 0; j < req.drm_prop_counts(i); j++) {
				auto prop = self->_device->getProperty(req.drm_props(prop_count + j));
				assert(prop);
				auto value = req.drm_prop_values(prop_count + j);

				auto prop_type = prop->propertyType();

				if(std::holds_alternative<IntPropertyType>(prop_type)) {
					assignments.push_back(Assignment::withInt(mode_obj, prop.get(), value));
				} else if(std::holds_alternative<BlobPropertyType>(prop_type)) {
					auto blob = self->_device->findBlob(value);

					assignments.push_back(Assignment::withBlob(mode_obj, prop.get(), blob));
				} else if(std::holds_alternative<ObjectPropertyType>(prop_type)) {
					auto obj = self->_device->findObject(value);

					assignments.push_back(Assignment::withModeObj(mode_obj, prop.get(), obj));
				}
			}

			prop_count += req.drm_prop_counts(i);
		}

		{
			auto valid = config->capture(assignments, state);
			assert(valid);
		}

		if(!(req.drm_flags() & DRM_MODE_ATOMIC_TEST_ONLY)) {
			config->commit(state);
			co_await config->waitForCompletion();
		}

		if(req.drm_flags() & DRM_MODE_PAGE_FLIP_EVENT) {
			assert(crtc_ids.size() == 1);
			self->_retirePageFlip(std::move(config), req.drm_cookie(), crtc_ids.front());
		}

		resp.set_error(managarm::fs::Errors::SUCCESS);

send:
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else{
		std::cout << "\e[31m" "core/drm: Unknown ioctl() with ID "
				<< req.command() << "\e[39m" << std::endl;

		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
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
			uint64_t cookie, uint32_t crtc_id) {
	co_await config->waitForCompletion();

	Event event;
	event.cookie = cookie;
	event.crtcId = crtc_id;
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

