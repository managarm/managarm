#pragma once

#include <stdint.h>

namespace eir {

constexpr uint32_t MB2_MAGIC = 0x36d76289;

struct Mb2Info {
	uint32_t size;
	uint32_t reserved;
	uint8_t tags[];
};

struct Mb2Tag {
	uint32_t type;
	uint32_t size;
	uint8_t data[];
};

struct Mb2TagModule {
	uint32_t type;
	uint32_t size;

	uint32_t start;
	uint32_t end;
	char string[];
};

struct Mb2Colour {
	uint8_t red;
	uint8_t green;
	uint8_t blue;
};

struct Mb2TagFramebuffer {
	uint32_t type;
	uint32_t size;

	uint64_t address;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	uint8_t bpp;

	static constexpr uint8_t kFramebufferTypeIndexed = 0;
	static constexpr uint8_t kFramebufferTypeRgb = 1;
	static constexpr uint8_t kFramebufferTypeEgaText = 2;

	uint8_t framebuffer_type;
	uint16_t reserved;

	union {
		struct {
			uint16_t palette_num_colors;
			Mb2Colour palette[];
		};
		struct {
			uint8_t red_field_position;
			uint8_t red_mask_size;
			uint8_t green_field_position;
			uint8_t green_mask_size;
			uint8_t blue_field_position;
			uint8_t blue_mask_size;
		};
	};
};

struct Mb2MmapEntry {
	uint64_t base;
	uint64_t length;
	uint32_t type;
	uint32_t reserved;
};

struct Mb2TagMmap {
	uint32_t type;
	uint32_t length;

	uint32_t entry_size;
	uint32_t entry_version;

	Mb2MmapEntry entries[];
};

struct Mb2TagCmdline {
	uint32_t type;
	uint32_t length;

	char string[];
};

struct Mb2TagRSDP {
	uint32_t type;
	uint32_t length;

	uint8_t data[];
};

enum {
	kMb2TagEnd = 0,
	kMb2TagCmdline = 1,
	kMb2TagBootloaderName = 2,
	kMb2TagModule = 3,
	kMb2TagBasicMeminfo = 4,
	kMb2TagBootDev = 5,
	kMb2TagMmap = 6,
	kMb2TagVbe = 7,
	kMb2TagFramebuffer = 8,
	kMb2TagElFSections = 9,
	kMb2TagApm = 10,
	kMb2TagEfi32 = 11,
	kMb2TagEfi64 = 12,
	kMb2TagSmBios = 13,
	kMb2TagAcpiOld = 14,
	kMb2TagAcpiNew = 15,
	kMb2TagNetwork = 16,
	kMb2TagEfiMmap = 17,
	kMb2TagEfiBs = 18,
	kMb2TagEfi32ImageHandle = 19,
	kMb2TagEfi64ImageHandle = 20,
	kMb2TagLoadBaseAddr = 21
};

} // namespace eir
