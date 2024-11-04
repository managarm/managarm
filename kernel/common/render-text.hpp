#pragma once

#include <cstddef>
#include <stdint.h>
#include <utility>

constexpr uint32_t rgb(int r, int g, int b) { return (r << 16) | (g << 8) | b; }

inline constexpr uint32_t rgbColor[16] = {
    rgb(1, 1, 1),
    rgb(222, 56, 43),
    rgb(57, 181, 74),
    rgb(255, 199, 6),
    rgb(0, 111, 184),
    rgb(118, 38, 113),
    rgb(44, 181, 233),
    rgb(204, 204, 204),
    rgb(128, 128, 128),
    rgb(255, 0, 0),
    rgb(0, 255, 0),
    rgb(255, 255, 0),
    rgb(0, 0, 255),
    rgb(255, 0, 255),
    rgb(0, 255, 255),
    rgb(255, 255, 255)
};

inline constexpr uint32_t defaultBg = rgb(16, 16, 16);

extern uint8_t fontBitmap[];

template <int FontWidth, int FontHeight>
void
renderChars(void *fb_ptr, unsigned int pitch, unsigned int x, unsigned int y, const char *c, int count, int fg, int bg, std::integral_constant<int, FontWidth>, std::integral_constant<int, FontHeight>) {
	auto fg_rgb = rgbColor[fg];
	auto bg_rgb = (bg < 0) ? defaultBg : rgbColor[bg];

	auto fb = reinterpret_cast<uint32_t *>(fb_ptr);
	auto line = fb + y * FontHeight * pitch + x * FontWidth;
	for (size_t i = 0; i < FontHeight; i++) {
		auto dest = line;
		for (int k = 0; k < count; k++) {
			auto dc = (c[k] >= 32 && c[k] <= 127) ? c[k] : 127;
			auto fontbits = fontBitmap[(dc - 32) * FontHeight + i];
			for (size_t j = 0; j < FontWidth; j++) {
				int bit = (1 << ((FontWidth - 1) - j));
				*dest++ = (fontbits & bit) ? fg_rgb : bg_rgb;
			}
		}
		line += pitch;
	}

	asm volatile("" : : : "memory");
}
