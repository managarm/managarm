#include <algorithm>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "utils.hpp"

namespace utils {

void toDrmModeInfo(const NvKmsKapiDisplayMode *displayMode, drm_mode_modeinfo *mi) {
	mi->clock = ((displayMode->timings.pixelClockHz + 500) / 1000);
	mi->hdisplay = static_cast<uint16_t>(displayMode->timings.hVisible);
	mi->hsync_start = static_cast<uint16_t>(displayMode->timings.hSyncStart);
	mi->hsync_end = static_cast<uint16_t>(displayMode->timings.hSyncEnd);
	mi->htotal = static_cast<uint16_t>(displayMode->timings.hTotal);
	mi->hskew = static_cast<uint16_t>(displayMode->timings.hSkew);
	mi->vdisplay = static_cast<uint16_t>(displayMode->timings.vVisible);
	mi->vsync_start = static_cast<uint16_t>(displayMode->timings.vSyncStart);
	mi->vsync_end = static_cast<uint16_t>(displayMode->timings.vSyncEnd);
	mi->vtotal = static_cast<uint16_t>(displayMode->timings.vTotal);
	mi->vrefresh = ((displayMode->timings.refreshRate + 500) / 1000);

	if (displayMode->timings.flags.interlaced)
		mi->flags |= DRM_MODE_FLAG_INTERLACE;

	if (displayMode->timings.flags.doubleScan)
		mi->flags |= DRM_MODE_FLAG_DBLSCAN;

	if (displayMode->timings.flags.hSyncPos)
		mi->flags |= DRM_MODE_FLAG_PHSYNC;

	if (displayMode->timings.flags.hSyncNeg)
		mi->flags |= DRM_MODE_FLAG_NHSYNC;

	if (displayMode->timings.flags.vSyncPos)
		mi->flags |= DRM_MODE_FLAG_PVSYNC;

	if (displayMode->timings.flags.vSyncNeg)
		mi->flags |= DRM_MODE_FLAG_NVSYNC;

	if (strlen(displayMode->name)) {
		memcpy(mi->name, displayMode->name, std::min(sizeof(mi->name), sizeof(displayMode->name)));

		mi->name[sizeof(mi->name) - 1] = '\0';
	} else {
		snprintf(
		    mi->name,
		    sizeof(mi->name),
		    "%dx%d%s",
		    mi->hdisplay,
		    mi->vdisplay,
		    (displayMode->timings.flags.interlaced) ? "i" : ""
		);
	}
}

void toNvModeInfo(const drm_mode_modeinfo *mi, NvKmsKapiDisplayMode *displayMode) {
	displayMode->timings.refreshRate = mi->vrefresh * 1000;
	displayMode->timings.pixelClockHz = mi->clock * 1000; /* In Hz */

	displayMode->timings.hVisible = mi->hdisplay;
	displayMode->timings.hSyncStart = mi->hsync_start;
	displayMode->timings.hSyncEnd = mi->hsync_end;
	displayMode->timings.hTotal = mi->htotal;
	displayMode->timings.hSkew = mi->hskew;

	displayMode->timings.vVisible = mi->vdisplay;
	displayMode->timings.vSyncStart = mi->vsync_start;
	displayMode->timings.vSyncEnd = mi->vsync_end;
	displayMode->timings.vTotal = mi->vtotal;

	if (mi->flags & DRM_MODE_FLAG_INTERLACE) {
		displayMode->timings.flags.interlaced = NV_TRUE;
	} else {
		displayMode->timings.flags.interlaced = NV_FALSE;
	}

	if (mi->flags & DRM_MODE_FLAG_DBLSCAN) {
		displayMode->timings.flags.doubleScan = NV_TRUE;
	} else {
		displayMode->timings.flags.doubleScan = NV_FALSE;
	}

	if (mi->flags & DRM_MODE_FLAG_PHSYNC) {
		displayMode->timings.flags.hSyncPos = NV_TRUE;
	} else {
		displayMode->timings.flags.hSyncPos = NV_FALSE;
	}

	if (mi->flags & DRM_MODE_FLAG_NHSYNC) {
		displayMode->timings.flags.hSyncNeg = NV_TRUE;
	} else {
		displayMode->timings.flags.hSyncNeg = NV_FALSE;
	}

	if (mi->flags & DRM_MODE_FLAG_PVSYNC) {
		displayMode->timings.flags.vSyncPos = NV_TRUE;
	} else {
		displayMode->timings.flags.vSyncPos = NV_FALSE;
	}

	if (mi->flags & DRM_MODE_FLAG_NVSYNC) {
		displayMode->timings.flags.vSyncNeg = NV_TRUE;
	} else {
		displayMode->timings.flags.vSyncNeg = NV_FALSE;
	}

	memcpy(displayMode->name, mi->name, std::min(sizeof(displayMode->name), sizeof(mi->name)));
}

} // namespace utils
