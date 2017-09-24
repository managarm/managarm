
#ifndef CORE_DRM_CORE_HPP
#define CORE_DRM_CORE_HPP

#include <queue>
#include <map>
#include <unordered_map>

#include <arch/mem_space.hpp>
#include <async/doorbell.hpp>
#include <async/mutex.hpp>
#include <async/result.hpp>

#include "id-allocator.hpp"
#include "range-allocator.hpp"

#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>

namespace drm_core {

struct ModeObject;
struct Crtc;
struct Encoder;
struct Connector;
struct Configuration;
struct FrameBuffer;
struct Plane;
struct Assignment;
struct Blob;

enum struct ObjectType {
	encoder,
	connector,
	crtc,
	frameBuffer,
	plane
};

struct Property {

};

struct BufferObject {
	BufferObject()
	: _mapping(-1) { }
	virtual std::shared_ptr<BufferObject> sharedBufferObject() = 0;
	virtual size_t getSize() = 0;
	virtual std::pair<helix::BorrowedDescriptor, uint64_t> getMemory() = 0;

	void setupMapping(uint64_t mapping);
	uint64_t getMapping();

private:
	uint64_t _mapping;
};

struct Blob {
	Blob(std::vector<char> data)
	: _data(std::move(data)) {  };

	size_t size();
	const void *data();
	
private:
	std::vector<char> _data;
};

struct Device {
	Device()
	: _mappingAllocator{63, 12} { }

	virtual std::unique_ptr<Configuration> createConfiguration() = 0;
	virtual std::pair<std::shared_ptr<BufferObject>, uint32_t> createDumb(uint32_t width,
			uint32_t height, uint32_t bpp) = 0;
	virtual std::shared_ptr<FrameBuffer> createFrameBuffer(std::shared_ptr<BufferObject> buff,
			uint32_t width, uint32_t height, uint32_t format, uint32_t pitch) = 0;
	
	void setupCrtc(std::shared_ptr<Crtc> crtc);
	void setupEncoder(std::shared_ptr<Encoder> encoder);
	void attachConnector(std::shared_ptr<Connector> connector);
	const std::vector<std::shared_ptr<Crtc>> &getCrtcs();
	const std::vector<std::shared_ptr<Encoder>> &getEncoders();
	const std::vector<std::shared_ptr<Connector>> &getConnectors();
	
	void registerObject(std::shared_ptr<ModeObject> object);
	ModeObject *findObject(uint32_t);

	uint64_t installMapping(drm_core::BufferObject *bo);
	std::pair<uint64_t, BufferObject *> findMapping(uint64_t offset);

	void setupMinDimensions(uint32_t width, uint32_t height);
	void setupMaxDimensions(uint32_t width, uint32_t height);
	uint32_t getMinWidth();
	uint32_t getMaxWidth();
	uint32_t getMinHeight();
	uint32_t getMaxHeight();
	
private:	
	std::vector<std::shared_ptr<Crtc>> _crtcs;
	std::vector<std::shared_ptr<Encoder>> _encoders;
	std::vector<std::shared_ptr<Connector>> _connectors;
	std::unordered_map<uint32_t, std::shared_ptr<ModeObject>> _objects;
	range_allocator _mappingAllocator;
	std::map<uint64_t, BufferObject *> _mappings;
	uint32_t _minWidth;
	uint32_t _maxWidth;
	uint32_t _minHeight;
	uint32_t _maxHeight;

public:
	id_allocator<uint32_t> allocator;
	Property srcWProperty;
	Property srcHProperty;
	Property fbIdProperty;
	Property modeIdProperty;
};

struct File {
	File(std::shared_ptr<Device> device)
	: _device(device) { };
	
	static async::result<size_t> read(std::shared_ptr<void> object, void *buffer, size_t length);
	static async::result<protocols::fs::AccessMemoryResult> accessMemory(std::shared_ptr<void> object, uint64_t, size_t);
	static async::result<void> ioctl(std::shared_ptr<void> object, managarm::fs::CntRequest req,
			helix::UniqueLane conversation);

	void attachFrameBuffer(std::shared_ptr<FrameBuffer> frame_buffer);
	const std::vector<std::shared_ptr<FrameBuffer>> &getFrameBuffers();
	
	uint32_t createHandle(std::shared_ptr<BufferObject> bo);
	BufferObject *resolveHandle(uint32_t handle);

private:
	std::vector<std::shared_ptr<FrameBuffer>> _frameBuffers;
	std::shared_ptr<Device> _device;
	std::unordered_map<uint32_t, std::shared_ptr<BufferObject>> _buffers;
	id_allocator<uint32_t> _allocator;
	
};

struct Configuration {
	virtual bool capture(std::vector<Assignment> assignment) = 0;
	virtual void dispose() = 0;
	virtual void commit() = 0;
};

struct ModeObject {
	ModeObject(ObjectType type, uint32_t id)
	: _type(type), _id(id) { };
	
	uint32_t id();
	Encoder *asEncoder();
	Connector *asConnector();
	Crtc *asCrtc();
	FrameBuffer *asFrameBuffer();
	Plane *asPlane();
	
private:
	ObjectType _type;
	uint32_t _id;
};

struct Crtc : ModeObject {
	Crtc(uint32_t id);
	virtual Plane *primaryPlane() = 0;
	
	std::shared_ptr<Blob> currentMode();
	void setCurrentMode(std::shared_ptr<Blob> mode);

	int index;

private:
	std::shared_ptr<Blob> _curMode;
};

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

struct Connector : ModeObject {
	Connector(uint32_t id);

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
	uint32_t connectorType();

private:
	std::vector<drm_mode_modeinfo> _modeList;
	drm_core::Encoder *_currentEncoder;
	uint32_t _currentStatus;
	std::vector<Encoder *> _possibleEncoders;
	uint32_t _physicalWidth;
	uint32_t _physicalHeight;
	uint32_t _subpixel;
	uint32_t _connectorType;
};

struct FrameBuffer : ModeObject {
	FrameBuffer(uint32_t id);
};

struct Plane : ModeObject {
	Plane(uint32_t id);	
};

struct Assignment {
	ModeObject *object;
	Property *property;
	uint64_t intValue;
	ModeObject *objectValue;
	std::shared_ptr<Blob> blobValue;
};

} //namespace drm_core

#endif // CORE_DRM_CORE_HPP

