#include <sys/epoll.h>

#include <helix/memory.hpp>
#include <libdrm/drm_fourcc.h>

#include "fs.bragi.hpp"
#include "posix.bragi.hpp"

#include "core/drm/mode-object.hpp"
#include "core/drm/debug.hpp"


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
// BufferObject
// ----------------------------------------------------------------

void drm_core::BufferObject::setupMapping(uint64_t mapping) {
	_mapping = mapping;
}

uint64_t drm_core::BufferObject::getMapping() {
	return _mapping;
}

uint32_t drm_core::BufferObject::getWidth() {
	return _width;
}

uint32_t drm_core::BufferObject::getHeight() {
	return _height;
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

	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->activeProperty(), drmState()->active));
	assignments.push_back(drm_core::Assignment::withBlob(this->sharedModeObject(), dev->modeIdProperty(), drmState()->mode));

	return assignments;
}

drm_core::CrtcState::CrtcState(std::weak_ptr<Crtc> crtc)
	: _crtc(crtc) {

}

std::weak_ptr<drm_core::Crtc> drm_core::CrtcState::crtc(void) {
	return _crtc;
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
	assignments.push_back(drm_core::Assignment::withModeObj(this->sharedModeObject(), dev->crtcIdProperty(), drmState()->crtc));
	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->srcHProperty(), drmState()->src_h << 16));
	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->srcWProperty(), drmState()->src_w << 16));
	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->crtcHProperty(), drmState()->crtc_h));
	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->crtcWProperty(), drmState()->crtc_w));
	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->srcXProperty(), drmState()->src_x << 16));
	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->srcYProperty(), drmState()->src_y << 16));
	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->crtcXProperty(), drmState()->crtc_x));
	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->crtcYProperty(), drmState()->crtc_y));
	assignments.push_back(drm_core::Assignment::withModeObj(this->sharedModeObject(), dev->fbIdProperty(), drmState()->fb));
	assignments.push_back(drm_core::Assignment::withBlob(this->sharedModeObject(), dev->inFormatsProperty(), drmState()->in_formats));

	return assignments;
}

drm_core::Plane::PlaneType drm_core::PlaneState::type(void) {
	return plane->type();
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

	assignments.push_back(drm_core::Assignment::withInt(this->sharedModeObject(), dev->dpmsProperty(), drmState()->dpms));
	assignments.push_back(drm_core::Assignment::withModeObj(this->sharedModeObject(), dev->crtcIdProperty(), drmState()->crtc));

	return assignments;
}

// ----------------------------------------------------------------
// Blob
// ----------------------------------------------------------------

uint32_t drm_core::Blob::id() {
	return _id;
}

size_t drm_core::Blob::size() {
	return _data.size();
}

const void *drm_core::Blob::data() {
	return _data.data();
}
