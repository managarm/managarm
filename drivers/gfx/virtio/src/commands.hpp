#pragma once

#include <async/result.hpp>

#include "src/virtio.hpp"

struct Cmd {
	static async::result<void> setScanout(uint32_t width, uint32_t height, uint32_t scanoutId, uint32_t resourceId, GfxDevice *device);
	static async::result<spec::DisplayInfo> getDisplayInfo(GfxDevice *device);
};
