#pragma once

#include <core/drm/core.hpp>
#include <protocols/hw/client.hpp>

#include <lil/intel.h>

struct GfxDevice final : drm_core::Device, std::enable_shared_from_this<GfxDevice> {
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
	protocols::hw::Device _hwDevice;
	range_allocator _vramAllocator;
	uint16_t _pchDevId;

public:
	struct BufferObject final : drm_core::BufferObject, std::enable_shared_from_this<BufferObject> {
		BufferObject(GfxDevice *device, GpuAddr gpu_addr, uint32_t width, uint32_t height, size_t size);

		std::shared_ptr<drm_core::BufferObject> sharedBufferObject() override;
		size_t getSize() override;
		std::pair<helix::BorrowedDescriptor, uint64_t> getMemory() override;

		GpuAddr getAddress();
	private:
		GfxDevice *_device;
		GpuAddr _gpuAddr;
		size_t _size;
		void *_ptr;
		helix::UniqueDescriptor _memoryView;
	};

	struct FrameBuffer final : drm_core::FrameBuffer {
		FrameBuffer(GfxDevice *device, std::shared_ptr<GfxDevice::BufferObject> bo,
				uint32_t pixel_pitch);

		void notifyDirty() override;
		uint32_t getWidth() override;
		uint32_t getHeight() override;

		GfxDevice::BufferObject *getBufferObject();
	private:
		std::shared_ptr<GfxDevice::BufferObject> _bo;
		uint32_t _pixelPitch;
	};

	struct Configuration : drm_core::Configuration {
		Configuration(GfxDevice *device)
		: _device(device) { };

		bool capture(std::vector<drm_core::Assignment> assignment, std::unique_ptr<drm_core::AtomicState> &state) override;
		void dispose() override;
		void commit(std::unique_ptr<drm_core::AtomicState> state) override;

	private:
		async::detached _doCommit(std::unique_ptr<drm_core::AtomicState> state);

		GfxDevice *_device;
	};

public:
	GfxDevice(protocols::hw::Device hw_device, uint16_t pch_devid);

	async::result<std::unique_ptr<drm_core::Configuration>> initialize();
};
