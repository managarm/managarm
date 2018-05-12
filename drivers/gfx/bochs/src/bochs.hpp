
#include <queue>
#include <map>
#include <unordered_map>

#include <arch/mem_space.hpp>
#include <async/doorbell.hpp>
#include <async/mutex.hpp>
#include <async/result.hpp>

#include "spec.hpp"

struct GfxDevice : drm_core::Device, std::enable_shared_from_this<GfxDevice> {
	struct FrameBuffer;

	struct Configuration : drm_core::Configuration {
		Configuration(GfxDevice *device)
		: _device(device), _width(0), _height(0), _fb(nullptr) { };
		
		bool capture(std::vector<drm_core::Assignment> assignment) override;
		void dispose() override;
		void commit() override;
		
	private:
		GfxDevice *_device;
		int _width;
		int _height;
		GfxDevice::FrameBuffer *_fb;
		std::shared_ptr<drm_core::Blob> _mode;
	};

	struct Plane : drm_core::Plane {
		Plane(GfxDevice *device);
	};
	
	struct BufferObject : drm_core::BufferObject, std::enable_shared_from_this<BufferObject> {
		BufferObject(GfxDevice *device, size_t alignment, size_t size,
				uintptr_t offset, ptrdiff_t displacement);
		
		std::shared_ptr<drm_core::BufferObject> sharedBufferObject() override;
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
		helix::UniqueDescriptor _memoryView;
	};

	struct Connector : drm_core::Connector {
		Connector(GfxDevice *device);

	private:
		std::vector<drm_core::Encoder *> _encoders;
	};

	struct Encoder : drm_core::Encoder {
		Encoder(GfxDevice *device);
	};
	
	struct Crtc : drm_core::Crtc {
		Crtc(GfxDevice *device);
		
		drm_core::Plane *primaryPlane() override;
	
	private:	
		GfxDevice *_device;
	};

	struct FrameBuffer : drm_core::FrameBuffer {
		FrameBuffer(GfxDevice *device, std::shared_ptr<GfxDevice::BufferObject> bo,
				uint32_t pixel_pitch);

		GfxDevice::BufferObject *getBufferObject();
		uint32_t getPixelPitch();
		void notifyDirty() override;

	private:
		std::shared_ptr<GfxDevice::BufferObject> _bo;
		uint32_t _pixelPitch;
	};

	GfxDevice(helix::UniqueDescriptor video_ram, void* frame_buffer);
	
	cofiber::no_future initialize();
	std::unique_ptr<drm_core::Configuration> createConfiguration() override;
	std::pair<std::shared_ptr<drm_core::BufferObject>, uint32_t> createDumb(uint32_t width,
			uint32_t height, uint32_t bpp) override;
	std::shared_ptr<drm_core::FrameBuffer> 
			createFrameBuffer(std::shared_ptr<drm_core::BufferObject> bo,
			uint32_t width, uint32_t height, uint32_t format, uint32_t pitch) override;
	
	//returns major, minor, patchlvl
	std::tuple<int, int, int> driverVersion() override;
	//returns name, desc, date
	std::tuple<std::string, std::string, std::string> driverInfo() override;
	
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

