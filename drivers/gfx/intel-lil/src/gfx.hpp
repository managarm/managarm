#pragma once

#include <arch/dma_pool.hpp>
#include <core/drm/core.hpp>
#include <protocols/hw/client.hpp>

#include <lil/intel.h>

struct GfxDevice final : drm_core::Device, std::enable_shared_from_this<GfxDevice> {
	std::unique_ptr<drm_core::Configuration> createConfiguration() override;
	std::pair<std::shared_ptr<drm_core::BufferObject>, uint32_t> createDumb(uint32_t width,
			uint32_t height, uint32_t bpp) override;
	std::shared_ptr<drm_core::FrameBuffer>
			createFrameBuffer(std::shared_ptr<drm_core::BufferObject> bo,
			uint32_t width, uint32_t height, uint32_t format, uint32_t pitch, uint32_t modifier) override;

	//returns major, minor, patchlvl
	std::tuple<int, int, int> driverVersion() override;
	//returns name, desc, date
	std::tuple<std::string, std::string, std::string> driverInfo() override;

private:
	protocols::hw::Device _hwDevice;
	range_allocator _vramAllocator;
	LilGpu _gpu;
	uint16_t _pchDevId;
	helix::UniqueDescriptor apertureHandle_;
	arch::contiguous_pool _pool = {};

	arch::dma_buffer gttScratch_;

public:
	struct BufferObject final : drm_core::BufferObject, std::enable_shared_from_this<BufferObject> {
		BufferObject(
		    GfxDevice *device,
		    GpuAddr gpu_addr,
		    helix::UniqueDescriptor allocationHandle,
		    helix::UniqueDescriptor apertureHandle,
		    uint32_t width,
		    uint32_t height,
		    size_t size
		);

		std::shared_ptr<drm_core::BufferObject> sharedBufferObject() override;
		size_t getSize() override;
		std::pair<helix::BorrowedDescriptor, uint64_t> getMemory() override;

		GpuAddr getAddress();
	private:
		[[maybe_unused]] GfxDevice *_device;
		GpuAddr _gpuAddr;
		size_t _size;
		helix::UniqueDescriptor allocationHandle_;
		helix::UniqueDescriptor apertureHandle_;
	};

	struct FrameBuffer final : drm_core::FrameBuffer {
		FrameBuffer(GfxDevice *device, std::shared_ptr<GfxDevice::BufferObject> bo,
				uint32_t pixel_pitch);

		void notifyDirty() override;
		uint32_t getWidth() override;
		uint32_t getHeight() override;
		uint32_t getModifier() override;

		GfxDevice::BufferObject *getBufferObject();
	private:
		std::shared_ptr<GfxDevice::BufferObject> _bo;
		uint32_t _pixelPitch;
	};

	struct Connector : drm_core::Connector {
		Connector(GfxDevice *device, LilConnector *lil);

		LilConnector *_lil;
	private:
		std::vector<drm_core::Encoder *> _encoders;
	};

	struct Encoder : drm_core::Encoder {
		Encoder(GfxDevice *device);
	};

	struct Plane : drm_core::Plane {
		Plane(GfxDevice *device, PlaneType type, LilPlane *lil);

		LilPlane *_lil;
	};

	struct Crtc final : drm_core::Crtc {
		Crtc(GfxDevice *device, LilCrtc *lil, Plane *primaryPlane);

		drm_core::Plane *primaryPlane() override;

		LilCrtc *_lil;
	private:
		GfxDevice *_device;
		Plane *_primaryPlane;
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

private:
	std::vector<std::shared_ptr<Plane>> planes_;
	std::vector<std::shared_ptr<Crtc>> crtcs_;
	std::vector<std::shared_ptr<Connector>> connectors_;
	std::vector<std::shared_ptr<Encoder>> encoders_;
	std::vector<std::shared_ptr<BufferObject>> bos_;

public:
	GfxDevice(protocols::hw::Device hw_device, uint16_t pch_devid);

	async::result<std::unique_ptr<drm_core::Configuration>> initialize();
};

template<>
struct std::formatter<LilTranscoder> : std::formatter<string_view> {
	auto format(const LilTranscoder& t, std::format_context& ctx) const {
		std::string temp;

		switch(t) {
			case TRANSCODER_A: temp = "TRANS_A"; break;
			case TRANSCODER_B: temp = "TRANS_B"; break;
			case TRANSCODER_C: temp = "TRANS_C"; break;
			case TRANSCODER_EDP: temp = "TRANS_EDP"; break;
			default: temp = "TRANS_INVALID"; break;
		}

		return std::formatter<string_view>::format(temp, ctx);
	}
};

template<>
struct std::formatter<GfxDevice::Crtc *> : std::formatter<string_view> {
	auto format(const GfxDevice::Crtc *crtc, std::format_context& ctx) const {
		std::string temp;
		std::format_to(std::back_inserter(temp), "Crtc(Transcoder={}, Pipe={}, planes={})",
			crtc->_lil->transcoder, crtc->_lil->pipe_id, crtc->_lil->num_planes);

		return std::formatter<string_view>::format(temp, ctx);
	}
};
