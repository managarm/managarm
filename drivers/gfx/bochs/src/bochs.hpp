
#include <queue>
#include <map>
#include <unordered_map>

#include <arch/mem_space.hpp>
#include <async/doorbell.hpp>
#include <async/mutex.hpp>
#include <async/result.hpp>

#include "spec.hpp"

// ----------------------------------------------------------------
// Sequential ID allocator
// ----------------------------------------------------------------

#include <assert.h>
#include <limits>
#include <set>

// Allocator for integral IDs. Provides O(log n) allocation and deallocation.
// Allocation always returns the smallest available ID.
template<typename T>
struct id_allocator {
private:
	struct node {
		T lb;
		T ub;

		friend bool operator< (const node &u, const node &v) {
			return u.lb < v.lb;
		}
	};

public:
	id_allocator(T lb = 1, T ub = std::numeric_limits<T>::max()) {
		_nodes.insert(node{lb, ub});
	}

	T allocate() {
		assert(!_nodes.empty());
		auto it = _nodes.begin();
		auto id = it->lb;
		if(it->lb < it->ub)
			_nodes.insert(std::next(it), node{it->lb + 1, it->ub});
		_nodes.erase(it);
		return id;
	}

	void free(T id) {
		// TODO: We could coalesce multiple adjacent nodes here.
		_nodes.insert(node{id, id});
	}

private:
	std::set<node> _nodes;
};

// ----------------------------------------------------------------
// Range allocator.
// ----------------------------------------------------------------

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <set>

struct range_allocator {
private:
	struct node {
		uint64_t off;
		unsigned int ord;

		friend bool operator< (const node &u, const node &v) {
			if(u.ord != v.ord)
				return u.ord < v.ord;
			return u.off < v.off;
		}
	};
	
	static unsigned int clz(unsigned long x) {
		return __builtin_clzl(x);
	}

public:
	static unsigned int round_order(size_t size) {
		assert(size >= 1);
		if(size == 1)
			return 0;
		return CHAR_BIT * sizeof(size_t) - clz(size - 1);
	}

	range_allocator(unsigned int order, unsigned int granularity)
	: _granularity{granularity} {
		_nodes.insert(node{0, order});
	}

	uint64_t allocate(size_t size) {
		return allocate_order(std::max(_granularity, round_order(size)));
	}

	uint64_t allocate_order(unsigned int order) {
		assert(order >= _granularity);

		auto it = _nodes.lower_bound(node{0, order});
		assert(it != _nodes.end());

		auto offset = it->off;

		while(it->ord != order) {
			assert(it->ord > order);
			auto high = _nodes.insert(it,
					node{it->off + (uint64_t(1) << (it->ord - 1)), it->ord - 1});
			auto low = _nodes.insert(high, node{it->off, it->ord - 1});
			_nodes.erase(it);
			it = low;
		}
		_nodes.erase(it);

		return offset;
	}

	void free(uint64_t offset, size_t size) {
		return free_order(offset, std::max(_granularity, round_order(size)));
	}

	void free_order(uint64_t offset, unsigned int order) {
		assert(order >= _granularity);

		_nodes.insert(node{offset, order});
	}

private:
	std::set<node> _nodes;
	unsigned int _granularity;
};

// ----------------------------------------------------------------
// Stuff that belongs in a DRM library.
// ----------------------------------------------------------------
namespace drm_backend {

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

	uint64_t installMapping(drm_backend::BufferObject *bo);
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
	
	drm_backend::Crtc *currentCrtc();
	void setCurrentCrtc(drm_backend::Crtc *crtc);
	void setupEncoderType(uint32_t type);
	uint32_t getEncoderType();
	void setupPossibleCrtcs(std::vector<Crtc *> crtcs);
	const std::vector<Crtc *> &getPossibleCrtcs();
	void setupPossibleClones(std::vector<Encoder *> clones);
	const std::vector<Encoder *> &getPossibleClones();

	int index;

private:
	drm_backend::Crtc *_currentCrtc;
	uint32_t _encoderType;
	std::vector<Crtc *> _possibleCrtcs;
	std::vector<Encoder *> _possibleClones;
};

struct Connector : ModeObject {
	Connector(uint32_t id);

	const std::vector<drm_mode_modeinfo> &modeList();
	void setModeList(std::vector<drm_mode_modeinfo> mode_list);
	
	drm_backend::Encoder *currentEncoder();
	void setCurrentEncoder(drm_backend::Encoder *encoder);

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
	drm_backend::Encoder *_currentEncoder;
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

}

// ----------------------------------------------------------------

struct GfxDevice : drm_backend::Device, std::enable_shared_from_this<GfxDevice> {
	struct FrameBuffer;

	struct Configuration : drm_backend::Configuration {
		Configuration(GfxDevice *device)
		: _device(device), _width(0), _height(0), _fb(nullptr) { };
		
		bool capture(std::vector<drm_backend::Assignment> assignment) override;
		void dispose() override;
		void commit() override;
		
	private:
		GfxDevice *_device;
		int _width;
		int _height;
		GfxDevice::FrameBuffer *_fb;
		std::shared_ptr<drm_backend::Blob> _mode;
	};

	struct Plane : drm_backend::Plane {
		Plane(GfxDevice *device);
	};
	
	struct BufferObject : drm_backend::BufferObject, std::enable_shared_from_this<BufferObject> {
		BufferObject(GfxDevice *device, size_t alignment, size_t size,
				uintptr_t offset, ptrdiff_t displacement)
		: _device{device}, _alignment{alignment}, _size{size},
				_offset{offset}, _displacement{displacement} { };
		
		std::shared_ptr<drm_backend::BufferObject> sharedBufferObject() override;
		size_t getSize() override;
		std::pair<helix::BorrowedDescriptor, uint64_t> getMemory() override;
		
		size_t getAlignment();
		uintptr_t getAddress();
		
	private:
		GfxDevice *_device;
		size_t _alignment;
		size_t _size;
		uintptr_t _offset;
		ptrdiff_t _displacement;
	};

	struct Connector : drm_backend::Connector {
		Connector(GfxDevice *device);

	private:
		std::vector<drm_backend::Encoder *> _encoders;
	};

	struct Encoder : drm_backend::Encoder {
		Encoder(GfxDevice *device);
	};
	
	struct Crtc : drm_backend::Crtc {
		Crtc(GfxDevice *device);
		
		drm_backend::Plane *primaryPlane() override;
	
	private:	
		GfxDevice *_device;
	};

	struct FrameBuffer : drm_backend::FrameBuffer {
		FrameBuffer(GfxDevice *device, std::shared_ptr<GfxDevice::BufferObject> bo,
				uint32_t pixel_pitch);

		GfxDevice::BufferObject *getBufferObject();
		uint32_t getPixelPitch();

	private:
		std::shared_ptr<GfxDevice::BufferObject> _bo;
		uint32_t _pixelPitch;
	};

	GfxDevice(helix::UniqueDescriptor video_ram, void* frame_buffer);
	
	cofiber::no_future initialize();
	std::unique_ptr<drm_backend::Configuration> createConfiguration() override;
	std::pair<std::shared_ptr<drm_backend::BufferObject>, uint32_t> createDumb(uint32_t width,
			uint32_t height, uint32_t bpp) override;
	std::shared_ptr<drm_backend::FrameBuffer> 
			createFrameBuffer(std::shared_ptr<drm_backend::BufferObject> bo,
			uint32_t width, uint32_t height, uint32_t format, uint32_t pitch) override;
	
private:
	std::shared_ptr<Crtc> _theCrtc;
	std::shared_ptr<Encoder> _theEncoder;
	std::shared_ptr<Connector> _theConnector;
	std::shared_ptr<Plane> _primaryPlane;

public:
	// FIX ME: this is a hack	
	helix::UniqueDescriptor _videoRam;

private:
	range_allocator _vramAllocator;
	arch::io_space _operational;
	void* _frameBuffer;
};

