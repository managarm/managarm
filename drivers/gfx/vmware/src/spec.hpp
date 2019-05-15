#ifndef VMWARE_SPEC_HPP
#define VMWARE_SPEC_HPP

namespace ports {
	arch::scalar_register<uint32_t> register_port(0x00);
	arch::scalar_register<uint32_t> value_port(0x01);
	arch::scalar_register<uint32_t> bios_port(0x02);
	arch::scalar_register<uint32_t> irq_status_port(0x08);
};

namespace versions {
	constexpr uint32_t magic = 0x900000;
	constexpr uint32_t id_2 = (magic << 8) | 2;
	constexpr uint32_t id_1 = (magic << 8) | 1;
	constexpr uint32_t id_0 = (magic << 8) | 0;
}

enum class register_index : uint32_t {
	id = 0,
	enable = 1,
	width = 2,
	height = 3,
	max_width = 4,
	max_height = 5,
	depth = 6,
	bits_per_pixel = 7,
	pseudocolor = 8,
	red_mask = 9,
	green_mask = 10,
	blue_mask = 11,
	bytes_per_line = 12,
	fb_start = 13,
	fb_offset = 14,
	vram_size = 15,
	fb_size = 16,

	capabilities = 17,
	mem_start = 18,
	mem_size = 19,
	config_done = 20,
	sync = 21,
	busy = 22,
	guest_id = 23,
	cursor_id = 24,
	cursor_x = 25,
	cursor_y = 26,
	cursor_on = 27,
	host_bits_per_pixel = 28,
	scratch_size = 29,
	mem_regs = 30,
	num_displays = 31,
	pitchlock = 32,
	irqmask = 33,

	num_guest_displays = 34,
	display_id = 35,
	display_is_primary = 36,
	display_position_x = 37,
	display_position_y = 38,
	display_width = 39,
	display_height = 40,

	gmr_id = 41,
	gmr_descriptor = 42,
	gmr_max_ids = 43,
	gmr_max_descriptor_length = 44,

	traces = 45,
	gmrs_max_pages = 46,
	memory_size = 47,
	top = 48,
};

enum class command_index : uint32_t {
	invalid_cmd           = 0,
	update                = 1,
	rect_copy             = 3,
	define_cursor         = 19,
	define_alpha_cursor   = 22,
	update_verbose        = 25,
	front_rop_fill        = 29,
	fence                 = 30,
	escape                = 33,
	define_screen         = 34,
	destroy_screen        = 35,
	define_gmrfb          = 36,
	blit_gmrfb_to_screen  = 37,
	blit_screen_to_gmrfb  = 38,
	annotation_fill       = 39,
	annotation_copy       = 40,
	define_gmr2           = 41,
	remap_gmr2            = 42,
	max
};

enum class fifo_index : uint32_t {
	min = 0,
	max,
	next_cmd,
	stop,

	capabilities = 4,
	flags,
	fence,

	_3d_hwversion,
	pitchlock,

	cursor_on,
	cursor_x,
	cursor_y,
	cursor_count,
	cursor_last_updated,

	reserved,

	cursor_screen_id,

	dead,

	_3d_hwversion_revised,

	_3d_caps      = 32,
	_3d_caps_last = 32 + 255,

	guest_3d_hwversion,
	fence_goal,
	busy,

	num_regs
};

// only necessary commands are implemented
namespace commands {
	struct define_alpha_cursor {
		uint32_t id;				// must be 0
		uint32_t hotspot_x;
		uint32_t hotspot_y;
		uint32_t width;
		uint32_t height;
		uint8_t pixel_data[];
	};

	struct define_cursor {
		uint32_t id;				// must be 0
		uint32_t hotspot_x;
		uint32_t hotspot_y;
		uint32_t width;
		uint32_t height;
		uint32_t _unknown;
		uint32_t bpp;
		uint8_t pixel_data[];
	};
}

enum class caps : uint32_t {
	cursor = 0x00000020,
	fifo_extended = 0x00008000,
	irqmask = 0x00040000,
	fifo_reserve = (1<<6),
	fifo_cursor_bypass_3 = (1<<4),
};

#endif
