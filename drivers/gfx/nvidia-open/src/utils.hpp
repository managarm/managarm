#pragma once

#include <libdrm/drm_mode.h>

extern "C" {

#include <nvkms-kapi.h>

}

namespace utils {

void toDrmModeInfo(const NvKmsKapiDisplayMode *displayMode, drm_mode_modeinfo *mi);
void toNvModeInfo(const drm_mode_modeinfo *mi, NvKmsKapiDisplayMode *displayMode);

} // namespace utils
