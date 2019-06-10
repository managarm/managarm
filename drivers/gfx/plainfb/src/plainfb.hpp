#ifndef DRIVERS_GFX_PLAINFB_PLAINFB_HPP
#define DRIVERS_GFX_PLAINFB_PLAINFB_HPP

#include <queue>
#include <map>
#include <unordered_map>

#include <arch/mem_space.hpp>
#include <async/doorbell.hpp>
#include <async/jump.hpp>
#include <async/mutex.hpp>
#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>

struct GfxDevice final : drm_core::Device, std::enable_shared_from_this<GfxDevice> {
	struct FrameBuffer;

	struct ScanoutState {
		ScanoutState()
		: fb{nullptr}, width(0), height(0) { };

		GfxDevice::FrameBuffer *fb;
		std::shared_ptr<drm_core::Blob> mode;
		int width;
		int height;
	};

	struct Configuration : drm_core::Configuration {
		Configuration(GfxDevice *device)
		: _device(device) { };
		
		bool capture(std::vector<drm_core::Assignment> assignment) override;
		void dispose() override;
		void commit() override;
		
	private:
		cofiber::no_future _dispatch();
	
		GfxDevice *_device;
		std::optional<ScanoutState> _state;
	};

	struct Plane : drm_core::Plane {
		Plane(GfxDevice *device);
	};
	
	struct BufferObject final : drm_core::BufferObject, std::enable_shared_from_this<BufferObject> {
		BufferObject(GfxDevice *device, size_t size, helix::UniqueDescriptor memory,
			uint32_t width, uint32_t height);
		
		std::shared_ptr<drm_core::BufferObject> sharedBufferObject() override;
		size_t getSize() override;
		std::pair<helix::BorrowedDescriptor, uint64_t> getMemory() override;
		uint32_t getWidth();
		uint32_t getHeight();
		void *accessMapping();
	
	private:
		GfxDevice *_device;
		size_t _size;
		helix::UniqueDescriptor _memory;
		uint32_t _width;
		uint32_t _height;
		helix::Mapping _bufferMapping;
	};

	struct Connector : drm_core::Connector {
		Connector(GfxDevice *device);
	};

	struct Encoder : drm_core::Encoder {
		Encoder(GfxDevice *device);
	};
	
	struct Crtc final : drm_core::Crtc {
		Crtc(GfxDevice *device, std::shared_ptr<Plane> plane);
		
		drm_core::Plane *primaryPlane() override;
		// cursorPlane
	
	private:	
		GfxDevice *_device;
		std::shared_ptr<Plane> _primaryPlane;
	};

	struct FrameBuffer final : drm_core::FrameBuffer {
		FrameBuffer(GfxDevice *device, std::shared_ptr<GfxDevice::BufferObject> bo,
				size_t pitch);

		size_t getPitch();
		bool fastScanout() { return _fastScanout; }

		GfxDevice::BufferObject *getBufferObject();
		void notifyDirty() override;

	private:
		GfxDevice *_device;
		std::shared_ptr<GfxDevice::BufferObject> _bo;
		size_t _pitch;
		bool _fastScanout = true;
	};

	GfxDevice(protocols::hw::Device hw_device,
			unsigned int screen_width, unsigned int screen_height,
			size_t screen_pitch, helix::Mapping fb_mapping);

	cofiber::no_future initialize();
	std::unique_ptr<drm_core::Configuration> createConfiguration() override;
	std::pair<std::shared_ptr<drm_core::BufferObject>, uint32_t> createDumb(uint32_t width,
			uint32_t height, uint32_t bpp) override;
	std::shared_ptr<drm_core::FrameBuffer> 
			createFrameBuffer(std::shared_ptr<drm_core::BufferObject> bo,
			uint32_t width, uint32_t height, uint32_t format, uint32_t pitch) override;
	
	std::tuple<int, int, int> driverVersion() override;
	std::tuple<std::string, std::string, std::string> driverInfo() override;

private:
	protocols::hw::Device _hwDevice;
	unsigned int _screenWidth;
	unsigned int _screenHeight;
	size_t _screenPitch;
	helix::Mapping _fbMapping;

	std::shared_ptr<Crtc> _theCrtc;
	std::shared_ptr<Encoder> _theEncoder;
	std::shared_ptr<Connector> _theConnector;

	bool _claimedDevice = false;
	bool _hardwareFbIsAligned = true;
};

#endif // DRIVERS_GFX_PLAINFB_PLAINFB_HPP
