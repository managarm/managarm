#pragma once

#include <async/result.hpp>

#include "src/virtio.hpp"

struct Cmd {
	static async::result<spec::DisplayInfo> getDisplayInfo(GfxDevice *device);
};
