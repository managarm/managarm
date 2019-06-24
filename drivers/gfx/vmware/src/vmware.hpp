
#include <queue>
#include <map>
#include <unordered_map>

#include <arch/mem_space.hpp>
#include <async/doorbell.hpp>
#include <async/mutex.hpp>
#include <async/result.hpp>

#include "spec.hpp"

struct GfxDevice final : drm_core::Device, std::enable_shared_from_this<GfxDevice> {
	struct FrameBuffer;

	struct Configuration : drm_core::Configuration {
		Configuration(GfxDevice *device)
		: _device(device), _width(0), _height(0), _fb(nullptr),
		_cursorWidth(0), _cursorHeight(0), _cursorX(0), _cursorY(0),
		_cursorFb(nullptr), _cursorUpdate(false), _cursorMove(false) { };

		bool capture(std::vector<drm_core::Assignment> assignment) override;
		void dispose() override;
		void commit() override;

	private:
		cofiber::no_future commitConfiguration();

		GfxDevice *_device;
		int _width;
		int _height;
		GfxDevice::FrameBuffer *_fb;
		std::shared_ptr<drm_core::Blob> _mode;

		int _cursorWidth;
		int _cursorHeight;
		uint64_t _cursorX;
		uint64_t _cursorY;
		GfxDevice::FrameBuffer *_cursorFb;
		bool _cursorUpdate;
		bool _cursorMove;
	};

	struct Plane : drm_core::Plane {
		Plane(GfxDevice *device);
	};

	struct BufferObject final : drm_core::BufferObject, std::enable_shared_from_this<BufferObject> {
		BufferObject(GfxDevice *device, size_t size, helix::UniqueDescriptor mem);

		std::shared_ptr<drm_core::BufferObject> sharedBufferObject() override;
		size_t getSize() override;
		std::pair<helix::BorrowedDescriptor, uint64_t> getMemory() override;
	private:
		size_t _size;
		helix::UniqueDescriptor _mem;
	};

	struct Connector : drm_core::Connector {
		Connector(GfxDevice *device);

	private:
		std::vector<drm_core::Encoder *> _encoders;
	};

	struct Encoder : drm_core::Encoder {
		Encoder(GfxDevice *device);
	};

	struct Crtc final : drm_core::Crtc {
		Crtc(GfxDevice *device);

		drm_core::Plane *primaryPlane() override;
		drm_core::Plane *cursorPlane() override;

	private:
		GfxDevice *_device;
	};

	struct FrameBuffer final : drm_core::FrameBuffer {
		FrameBuffer(GfxDevice *device, std::shared_ptr<GfxDevice::BufferObject> bo,
				uint32_t pixel_pitch);

		GfxDevice::BufferObject *getBufferObject();
		uint32_t getPixelPitch();
		void notifyDirty() override;

	private:
		std::shared_ptr<GfxDevice::BufferObject> _bo;
		uint32_t _pixelPitch;
	};

	struct DeviceFifo {
		DeviceFifo(GfxDevice *device, helix::Mapping fifoMapping);

		void initialize();
		async::result<void> defineCursor(int width, int height, GfxDevice::BufferObject *bo);
		void moveCursor(int x, int y);
		void setCursorState(bool enabled);
		bool hasCapability(caps capability);

	private:
		async::result<void *> reserve(size_t size);
		void commit(size_t);
		void commitAll();
		void writeRegister(fifo_index idx, uint32_t value);
		uint32_t readRegister(fifo_index idx);

		GfxDevice *_device;
		helix::Mapping _fifoMapping;

		size_t _reservedSize;
		size_t _fifoSize;

		uint8_t _bounceBuf[1024 * 1024];
		bool _usingBounceBuf;
	};

	GfxDevice(protocols::hw::Device hw_device,
			helix::Mapping fb,
			helix::Mapping fifo,
			helix::UniqueDescriptor io_ports, uint16_t io_base);

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
	std::shared_ptr<Crtc> _crtc;
	std::shared_ptr<Encoder> _encoder;
	std::shared_ptr<Connector> _connector;
	std::shared_ptr<Plane> _primaryPlane;
	std::shared_ptr<Plane> _cursorPlane;

	uint32_t readRegister(register_index reg);
	void writeRegister(register_index reg, uint32_t value);
	bool hasCapability(caps capability);

	async::result<void> waitIrq(uint32_t irq_mask);

	protocols::hw::Device _hwDev;
	DeviceFifo _fifo;

	arch::io_space _operational;
	helix::Mapping _fbMapping;

	bool _isClaimed;
	uint32_t _deviceVersion;
	uint32_t _deviceCaps;
};
