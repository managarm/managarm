
#ifndef CORE_DRM_CORE_HPP
#define CORE_DRM_CORE_HPP

#include <queue>
#include <map>
#include <unordered_map>
#include <optional>

#include <arch/mem_space.hpp>
#include <async/cancellation.hpp>
#include <async/doorbell.hpp>
#include <async/mutex.hpp>
#include <async/result.hpp>
#include <helix/memory.hpp>

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

struct IntPropertyType {

};

struct ObjectPropertyType {

};

struct BlobPropertyType {
		
};

using PropertyType = std::variant<
	IntPropertyType,
	ObjectPropertyType,
	BlobPropertyType
>;

struct Event {
	uint64_t cookie;
	uint64_t timestamp;
};

struct Property {
	Property(PropertyType property_type)
	: _propertyType(property_type) { }

	virtual ~Property() = default;

	virtual bool validate(const Assignment& assignment);
	PropertyType propertyType();

private:
	PropertyType _propertyType;
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
	Blob(std::vector<char> data)
	: _data(std::move(data)) {  };

	size_t size();
	const void *data();
	
private:
	std::vector<char> _data;
};

struct Device {
	Device();

protected:
	~Device() = default;

public:
	virtual std::unique_ptr<Configuration> createConfiguration() = 0;
	virtual std::pair<std::shared_ptr<BufferObject>, uint32_t> createDumb(uint32_t width,
			uint32_t height, uint32_t bpp) = 0;
	virtual std::shared_ptr<FrameBuffer> createFrameBuffer(std::shared_ptr<BufferObject> buff,
			uint32_t width, uint32_t height, uint32_t format, uint32_t pitch) = 0;
	//returns major, minor, patchlvl
	virtual std::tuple<int, int, int> driverVersion() = 0;
	//returns name, desc, date
	virtual std::tuple<std::string, std::string, std::string> driverInfo() = 0;

	void setupCrtc(Crtc *crtc);
	void setupEncoder(Encoder *encoder);
	void attachConnector(Connector *connector);
	const std::vector<Crtc *> &getCrtcs();
	const std::vector<Encoder *> &getEncoders();
	const std::vector<Connector *> &getConnectors();
	
	void registerObject(ModeObject *object);
	std::shared_ptr<ModeObject> findObject(uint32_t);

	uint64_t installMapping(drm_core::BufferObject *bo);

	void setupMinDimensions(uint32_t width, uint32_t height);
	void setupMaxDimensions(uint32_t width, uint32_t height);
	uint32_t getMinWidth();
	uint32_t getMaxWidth();
	uint32_t getMinHeight();
	uint32_t getMaxHeight();

private:	
	std::vector<Crtc *> _crtcs;
	std::vector<Encoder *> _encoders;
	std::vector<Connector *> _connectors;
	std::unordered_map<uint32_t, ModeObject *> _objects;
	id_allocator<uint32_t> _memorySlotAllocator;
	std::map<uint64_t, BufferObject *> _mappings;
	uint32_t _minWidth;
	uint32_t _maxWidth;
	uint32_t _minHeight;
	uint32_t _maxHeight;
	std::shared_ptr<Property> _srcWProperty;
	std::shared_ptr<Property> _srcHProperty;
	std::shared_ptr<Property> _fbIdProperty;
	std::shared_ptr<Property> _modeIdProperty;
	std::shared_ptr<Property> _crtcXProperty;
	std::shared_ptr<Property> _crtcYProperty;

public:
	id_allocator<uint32_t> allocator;
	Property *srcWProperty();
	Property *srcHProperty();
	Property *fbIdProperty();
	Property *modeIdProperty();
	Property *crtcXProperty();
	Property *crtcYProperty();
};

struct File {
	File(std::shared_ptr<Device> device);

	static async::result<protocols::fs::ReadResult>
	read(void *object, const char *, void *buffer, size_t length);

	static async::result<protocols::fs::AccessMemoryResult>
	accessMemory(void *object, uint64_t, size_t);

	static async::result<void>
	ioctl(void *object, managarm::fs::CntRequest req, helix::UniqueLane conversation);

	static async::result<protocols::fs::PollResult>
	poll(void *object, uint64_t sequence, async::cancellation_token cancellation);

	void setBlocking(bool blocking);

	void attachFrameBuffer(std::shared_ptr<FrameBuffer> frame_buffer);
	void detachFrameBuffer(FrameBuffer *frame_buffer);
	const std::vector<std::shared_ptr<FrameBuffer>> &getFrameBuffers();
	
	uint32_t createHandle(std::shared_ptr<BufferObject> bo);
	BufferObject *resolveHandle(uint32_t handle);

	void postEvent(Event event);

	helix::BorrowedDescriptor statusPageMemory() {
		return _statusPage.getMemory();
	}

private:
	async::detached _retirePageFlip(std::unique_ptr<Configuration> config,
			uint64_t cookie);

	std::shared_ptr<Device> _device;

	helix::UniqueDescriptor _memory;

	std::vector<std::shared_ptr<FrameBuffer>> _frameBuffers;

	// BufferObjects associated with this file.
	std::unordered_map<uint32_t, std::shared_ptr<BufferObject>> _buffers;
	id_allocator<uint32_t> _allocator;

	// Event queuing structures.
	bool _isBlocking = true;
	std::deque<Event> _pendingEvents;
	uint64_t _eventSequence;
	async::doorbell _eventBell;

	protocols::fs::StatusPageProvider _statusPage;
};

struct Configuration {
	virtual ~Configuration() = default;

	virtual bool capture(std::vector<Assignment> assignment) = 0;
	virtual void dispose() = 0;
	virtual void commit() = 0;

	async::result<void> waitForCompletion() {
		return _promise.async_get();
	}

protected:
	// TODO: Let derive classes handle the promise?
	void complete() {
		_promise.set_value();
	}

private:
	async::promise<void> _promise;
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

	void setupWeakPtr(std::weak_ptr<ModeObject> self);
	std::shared_ptr<ModeObject> sharedModeObject();
	
private:
	ObjectType _type;
	uint32_t _id;
	std::weak_ptr<ModeObject> _self;
};

struct Crtc : ModeObject {
	Crtc(uint32_t id);

protected:
	~Crtc() = default;

public:
	virtual Plane *primaryPlane() = 0;
	virtual Plane *cursorPlane();

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

protected:
	~FrameBuffer() = default;

public:
	virtual void notifyDirty() = 0;
};

struct Plane : ModeObject {
	Plane(uint32_t id);	
};

struct Assignment {
	std::shared_ptr<ModeObject> object;
	Property *property;
	uint64_t intValue;
	std::shared_ptr<ModeObject> objectValue;
	std::shared_ptr<Blob> blobValue;
};

async::detached serveDrmDevice(std::shared_ptr<drm_core::Device> device,
		helix::UniqueLane lane);

// ---------------------------------------------
// Formats
// ---------------------------------------------

uint32_t convertLegacyFormat(uint32_t bpp, uint32_t depth);

struct FormatInfo {
	int cpp;
};

std::optional<FormatInfo> getFormatInfo(uint32_t fourcc);

drm_mode_modeinfo makeModeInfo(const char *name, uint32_t type,
		uint32_t clock, unsigned int hdisplay, unsigned int hsync_start,
		unsigned int hsync_end, unsigned int htotal, unsigned int hskew,
		unsigned int vdisplay, unsigned int vsync_start, unsigned int vsync_end,
		unsigned int vtotal, unsigned int vscan, uint32_t flags);

void addDmtModes(std::vector<drm_mode_modeinfo> &supported_modes,
		unsigned int max_width, unsigned max_height);

// Copies 16-byte aligned buffers. Expected to be faster than plain memcpy().
extern "C" void fastCopy16(void *, const void *, size_t);

} //namespace drm_core

#endif // CORE_DRM_CORE_HPP
