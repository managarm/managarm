#pragma once

#include <core/id-allocator.hpp>

#include "fwd-decls.hpp"

#include "device.hpp"
#include "range-allocator.hpp"
#include "property.hpp"

namespace drm_core {

enum struct ObjectType {
	encoder,
	connector,
	crtc,
	frameBuffer,
	plane
};

struct BufferObject {
	BufferObject()
	: _mapping(-1) { }

protected:
	~BufferObject() = default;

public:
	virtual std::shared_ptr<BufferObject> sharedBufferObject() = 0;
	virtual size_t getSize() = 0;
	virtual std::pair<helix::BorrowedDescriptor, uint64_t> getMemory() = 0;

	void setupMapping(uint64_t mapping);
	uint64_t getMapping();

private:
	uint64_t _mapping;
};

struct Blob {
	Blob(std::vector<char> data, uint32_t id)
	: _data(std::move(data)), _id(id) {  };

	uint32_t id();
	size_t size();
	const void *data();

private:
	std::vector<char> _data;
	uint32_t _id;
};

/**
 * A ModeObject represents modeset objects visible to userspace.
 * It can represent Connectors, CRTCs, Encoders, Framebuffers and Planes.
 */
struct ModeObject {
	ModeObject(ObjectType type, uint32_t id)
	: _type(type), _id(id) { };

	virtual ~ModeObject() = default;

	uint32_t id();
	ObjectType type();

	Encoder *asEncoder();
	Connector *asConnector();
	Crtc *asCrtc();
	FrameBuffer *asFrameBuffer();
	Plane *asPlane();

	void setupWeakPtr(std::weak_ptr<ModeObject> self);
	std::shared_ptr<ModeObject> sharedModeObject();

	/**
	 * Get a vector of assignments for the ModeObject
	 *
	 * @param dev
	 * @return std::vector<drm_core::Assignment>
	 */
	virtual std::vector<drm_core::Assignment> getAssignments(std::shared_ptr<Device> dev);
private:
	ObjectType _type;
	uint32_t _id;
	std::weak_ptr<ModeObject> _self;
};

struct CrtcState {
	CrtcState(std::weak_ptr<Crtc> crtc);

	std::weak_ptr<Crtc> crtc(void);

	std::weak_ptr<Crtc> _crtc;
	bool active = 0;

	bool planesChanged = 0;
	bool modeChanged = 0;
	bool activeChanged = 0;
	bool connectorsChanged = 0;
	uint32_t planeMask = 0;
	uint32_t connectorMask = 0;
	uint32_t encoderMask = 0;

	std::shared_ptr<Blob> mode;
};

struct Crtc : ModeObject {
	Crtc(uint32_t id);

protected:
	~Crtc() = default;

public:
	void setupState(std::shared_ptr<Crtc> crtc);

	virtual Plane *primaryPlane() = 0;
	virtual Plane *cursorPlane();

	std::shared_ptr<drm_core::CrtcState> drmState();
	void setDrmState(std::shared_ptr<drm_core::CrtcState> new_state);

	std::vector<drm_core::Assignment> getAssignments(std::shared_ptr<Device> dev);

	int index;

private:
	std::shared_ptr<CrtcState> _drmState;
};

/**
 * The Encoder is responsible for converting a frame into the appropriate format for a connector.
 * Together with a Connector, it forms what xrandr would understand as an output.
 */
struct Encoder : ModeObject {
	Encoder(uint32_t id);

	drm_core::Crtc *currentCrtc();
	void setCurrentCrtc(drm_core::Crtc *crtc);
	void setupEncoderType(uint32_t type);
	uint32_t getEncoderType();
	void setupPossibleCrtcs(std::vector<Crtc *> crtcs);
	const std::vector<Crtc *> &getPossibleCrtcs();
	void setupPossibleClones(std::vector<Encoder *> clones);
	const std::vector<Encoder *> &getPossibleClones();

	int index;

private:
	drm_core::Crtc *_currentCrtc;
	uint32_t _encoderType;
	std::vector<Crtc *> _possibleCrtcs;
	std::vector<Encoder *> _possibleClones;
};

struct ConnectorState {
	ConnectorState(std::weak_ptr<Connector> connector) : connector{connector}, crtc{}, encoder{} {};

	std::shared_ptr<Connector> connector;
	std::shared_ptr<Crtc> crtc;
	std::shared_ptr<Encoder> encoder;
	uint32_t dpms = 0;
};

/**
 * Represents a display connector.
 * It transmits the signal to the display, detects display connection and removal and exposes the display's supported modes.
 */
struct Connector : ModeObject {
	Connector(uint32_t id);

	void setupState(std::shared_ptr<drm_core::Connector> connector);

	const std::vector<drm_mode_modeinfo> &modeList();
	void setModeList(std::vector<drm_mode_modeinfo> mode_list);

	drm_core::Encoder *currentEncoder();
	void setCurrentEncoder(drm_core::Encoder *encoder);

	void setCurrentStatus(uint32_t status);
	uint32_t getCurrentStatus();

	void setupPossibleEncoders(std::vector<Encoder *> encoders);
	const std::vector<Encoder *> &getPossibleEncoders();

	void setupPhysicalDimensions(uint32_t width, uint32_t height);
	uint32_t getPhysicalWidth();
	uint32_t getPhysicalHeight();
	void setupSubpixel(uint32_t subpixel);
	uint32_t getSubpixel();
	void setConnectorType(uint32_t type);
	uint32_t connectorType();

	std::shared_ptr<drm_core::ConnectorState> drmState();
	void setDrmState(std::shared_ptr<drm_core::ConnectorState> new_state);

	std::vector<drm_core::Assignment> getAssignments(std::shared_ptr<Device> dev);

private:
	std::vector<drm_mode_modeinfo> _modeList;
	drm_core::Encoder *_currentEncoder;
	uint32_t _currentStatus;
	std::vector<Encoder *> _possibleEncoders;
	uint32_t _physicalWidth;
	uint32_t _physicalHeight;
	uint32_t _subpixel;
	uint32_t _connectorType;

	std::shared_ptr<ConnectorState> _drmState;
};

/**
 * Holds all info relating to a framebuffer, such as size and pixel format.
 */
struct FrameBuffer : ModeObject {
	FrameBuffer(uint32_t id);

protected:
	~FrameBuffer() = default;

public:
	virtual void notifyDirty() = 0;
};

struct Plane : ModeObject {
	enum struct PlaneType {
		OVERLAY = 0,
		PRIMARY = 1,
		CURSOR = 2,
	};

	Plane(uint32_t id, PlaneType type);

	void setupState(std::shared_ptr<Plane> plane);

	std::vector<drm_core::Assignment> getAssignments(std::shared_ptr<Device> dev);

	PlaneType type();

	void setCurrentFrameBuffer(drm_core::FrameBuffer *crtc);
	drm_core::FrameBuffer *getFrameBuffer();

	void setupPossibleCrtcs(std::vector<Crtc *> crtcs);
	const std::vector<Crtc *> &getPossibleCrtcs();

	std::shared_ptr<drm_core::PlaneState> drmState();
	void setDrmState(std::shared_ptr<drm_core::PlaneState> new_state);

private:
	PlaneType _type;
	drm_core::FrameBuffer *_fb;
	std::vector<Crtc *> _possibleCrtcs;
	std::shared_ptr<PlaneState> _drmState;
};

struct PlaneState {
	PlaneState(std::weak_ptr<Plane> plane) : plane{plane}, crtc{}, fb{} { };

	Plane::PlaneType type(void);

	std::shared_ptr<Plane> plane;
	std::shared_ptr<Crtc> crtc;
	std::shared_ptr<FrameBuffer> fb;
	int32_t crtc_x = 0;
	int32_t crtc_y = 0;
	uint32_t crtc_w = 0;
	uint32_t crtc_h = 0;
	uint32_t src_x = 0;
	uint32_t src_y = 0;
	uint32_t src_w = 0;
	uint32_t src_h = 0;
};

} //namespace drm_core
