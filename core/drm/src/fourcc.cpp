#include <array>
#include <optional>

#include <core/drm/core.hpp>
#include <drm/drm_fourcc.h>

namespace drm_core {

namespace {

std::array<const FormatInfo, 5> formats = {{
    {.format = DRM_FORMAT_RGB565, .has_alpha = false, .char_per_block = {2, 0, 0, 0}},
    {.format = DRM_FORMAT_XRGB8888, .has_alpha = false, .char_per_block = {4, 0, 0, 0}},
    {.format = DRM_FORMAT_XBGR8888, .has_alpha = false, .char_per_block = {4, 0, 0, 0}},
    {.format = DRM_FORMAT_ARGB8888, .has_alpha = true, .char_per_block = {4, 0, 0, 0}},
    {.format = DRM_FORMAT_ABGR8888, .has_alpha = true, .char_per_block = {4, 0, 0, 0}},
}};

}

std::optional<FormatInfo> getFormatInfo(uint32_t fourcc) {
	auto entry = std::find_if(formats.begin(), formats.end(), [&](const auto &e) {
		return e.format == fourcc;
	});

	if (!entry)
		return std::nullopt;

	return *entry;
}

uint8_t getFormatBlockHeight(const FormatInfo &info, size_t plane) {
	if (plane >= info.planes)
		return 0;

	if (!info.block_h[plane])
		return 1;

	return info.block_h[plane];
}

uint8_t getFormatBlockWidth(const FormatInfo &info, size_t plane) {
	if (plane >= info.planes)
		return 0;

	if (!info.block_w[plane])
		return 1;

	return info.block_w[plane];
}

uint8_t getFormatBpp(const FormatInfo &info, size_t plane) {
	if (plane >= info.planes)
		return 0;

	return info.char_per_block[plane] * 8 /
	       (getFormatBlockWidth(info, plane) * getFormatBlockHeight(info, plane));
}

} // namespace drm_core
