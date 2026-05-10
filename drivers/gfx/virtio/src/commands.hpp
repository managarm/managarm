#pragma once

#include <async/result.hpp>

#include "src/virtio.hpp"

struct Cmd {
	static async::result<void> transferToHost2d(uint32_t width, uint32_t height, uint32_t resourceId, GfxDevice *device);
	static async::result<void> setScanout(uint32_t width, uint32_t height, uint32_t scanoutId, uint32_t resourceId, GfxDevice *device);
	static async::result<void> resourceFlush(uint32_t width, uint32_t height, uint32_t resourceId, GfxDevice *device);
	static async::result<arch::dma_object<spec::DisplayInfo>> getDisplayInfo(GfxDevice *device);
	static async::result<void> create2d(uint32_t width, uint32_t height, uint32_t resourceId, GfxDevice *device);
	static async::result<void> attachBacking(uint32_t resourceId, arch::dma_buffer_view view, GfxDevice *device);
};
