
#include <map>
#include <queue>
#include <unordered_map>

#include <arch/io_space.hpp>
#include <arch/mem_space.hpp>
#include <async/mutex.hpp>
#include <async/recurring-event.hpp>
#include <async/result.hpp>
#include <core/drm/device.hpp>
#include <protocols/hw/client.hpp>

#include "spec.hpp"

struct GfxDevice final : drm_core::Device, std::enable_shared_from_this<GfxDevice> {
	struct FrameBuffer;

	struct Configuration : drm_core::Configuration {
		Configuration(GfxDevice *device)
		    : _device(device),
		      _cursorUpdate(false),
		      _cursorMove(false) {};

		bool capture(
		    std::vector<drm_core::Assignment> assignment,
		    std::unique_ptr<drm_core::AtomicState> &state
		) override;
		void dispose() override;
		void commit(std::unique_ptr<drm_core::AtomicState> state) override;

	  private:
		async::detached commitConfiguration(std::unique_ptr<drm_core::AtomicState> state);

		GfxDevice *_device;

		bool _cursorUpdate;
		bool _cursorMove;
	};

	struct Plane : drm_core::Plane {
		Plane(GfxDevice *device, PlaneType type);
	};

	struct BufferObject final : drm_core::BufferObject, std::enable_shared_from_this<BufferObject> {
		BufferObject(
		    GfxDevice *device,
		    size_t size,
		    helix::UniqueDescriptor mem,
		    uint32_t width,
		    uint32_t height
		);

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
		FrameBuffer(
		    GfxDevice *device, std::shared_ptr<GfxDevice::BufferObject> bo, uint32_t pixel_pitch
		);

		GfxDevice::BufferObject *getBufferObject();
		uint32_t getPixelPitch();
		void notifyDirty() override;
		uint32_t getWidth() override;
		uint32_t getHeight() override;

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
		async::result<void> updateRectangle(int x, int y, int w, int h);

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

	GfxDevice(
	    protocols::hw::Device hw_device,
	    helix::Mapping fb,
	    helix::Mapping fifo,
	    helix::UniqueDescriptor io_ports,
	    uint16_t io_base
	);

	async::result<std::unique_ptr<drm_core::Configuration>> initialize();
	std::unique_ptr<drm_core::Configuration> createConfiguration() override;
	std::pair<std::shared_ptr<drm_core::BufferObject>, uint32_t>
	createDumb(uint32_t width, uint32_t height, uint32_t bpp) override;
	std::shared_ptr<drm_core::FrameBuffer> createFrameBuffer(
	    std::shared_ptr<drm_core::BufferObject> bo,
	    uint32_t width,
	    uint32_t height,
	    uint32_t format,
	    uint32_t pitch
	) override;

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
