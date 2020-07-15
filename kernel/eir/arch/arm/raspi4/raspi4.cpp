#include <stdint.h>
#include <frigg/c-support.h>
#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>
#include <frigg/debug.hpp>
#include <frigg/support.hpp>
#include <frigg/string.hpp>
#include <eir/interface.hpp>
#include <frigg/libc.hpp>
#include <render-text.hpp>
#include "../cpio.hpp"

#include <arch/aarch64/mem_space.hpp>
#include <arch/register.hpp>

namespace aux {
	static constexpr arch::bit_register<uint32_t> enable{0x04};
}

// Mini usart
namespace aux_mu {
	static constexpr arch::scalar_register<uint32_t> data{0x40};
	static constexpr arch::bit_register<uint32_t> line_control{0x4c};
	static constexpr arch::bit_register<uint32_t> line_status{0x54};
	static constexpr arch::scalar_register<uint32_t> baudrate{0x68};
}

namespace aux_enable {
	static constexpr arch::field<uint32_t, bool> mini_usart{0, 1};
}

namespace aux_mu_lcr {
	static constexpr arch::field<uint32_t, bool> data_size{0, 1};
}

namespace aux_mu_lsr {
	static constexpr arch::field<uint32_t, bool> rx_full{0, 1};
	static constexpr arch::field<uint32_t, bool> tx_empty{5, 1};
}

// For raspi4
//static constexpr arch::mem_space mbox0_space{0xfe00b880};
// For raspi3
static constexpr arch::mem_space mbox0_space{0x3f00b880};

struct alignas(16) BcmFBInfo {
	uint32_t width;
	uint32_t height;
	uint32_t vwidth;
	uint32_t vheight;
	uint32_t pitch;
	uint32_t depth;
	uint32_t x;
	uint32_t y;
	uint32_t pointer;
	uint32_t size;
};

namespace mbox {
	static constexpr arch::bit_register<uint32_t> read{0x00};
	static constexpr arch::bit_register<uint32_t> status{0x18};
	static constexpr arch::bit_register<uint32_t> write{0x20};
}

enum class MboxChannel {
	pmi = 0,
	fb,
	vuart,
	vchiq,
	led,
	button,
	touch
};

namespace mbox_io {
	static constexpr arch::field<uint32_t, MboxChannel> channel{0, 4};
	static constexpr arch::field<uint32_t, uint32_t> value{4, 28};
}

namespace mbox_status {
	static constexpr arch::field<uint32_t, bool> empty{30, 1};
	static constexpr arch::field<uint32_t, bool> full{31, 1};
}

void eirMboxWrite(MboxChannel channel, uint32_t value) {
	while (mbox0_space.load(mbox::status) & mbox_status::full)
		;

	mbox0_space.store(mbox::write, mbox_io::channel(channel) | mbox_io::value(value >> 4));
}

uint32_t eirMboxRead(MboxChannel channel) {
	while (true) {
		while (mbox0_space.load(mbox::status) & mbox_status::empty)
			;

		auto f = mbox0_space.load(mbox::read);
		if ((f & mbox_io::channel) != channel)
			continue;

		return (f & mbox_io::value) << 4;
	}
}

struct OutputSink {
	// For raspi4:
	//static constexpr arch::mem_space aux_space{0xfe215000};
	// For raspi3:
	static constexpr arch::mem_space aux_space{0x3f215000};

	uint32_t fbWidth;
	uint32_t fbHeight;
	uint32_t fbPitch;
	void *fbPointer;

	uint32_t fbX;
	uint32_t fbY;

	static constexpr uint32_t fontWidth = 8;
	static constexpr uint32_t fontHeight = 16;

	void init() {
		constexpr uint32_t default_clock = 250000000;
		aux_space.store(aux::enable, aux_enable::mini_usart(true));
		aux_space.store(aux_mu::line_control, aux_mu_lcr::data_size(true)); // 8 bits
		aux_space.store(aux_mu::baudrate, default_clock / (8 * 115200) - 1);
	}

	void initFb(BcmFBInfo &info) {
		fbWidth = info.width;
		fbHeight = info.height;
		fbPitch = info.pitch;
		fbPointer = reinterpret_cast<void *>(info.pointer);

		fbX = 0;
		fbY = 0;
	}

	void print(char c) {
		while (!(aux_space.load(aux_mu::line_status) & aux_mu_lsr::tx_empty))
			;

		aux_space.store(aux_mu::data, c);

		if (fbPointer) {
			if (c == '\n') {
				fbX = 0;
				fbY++;
			} else if (fbX >= fbWidth / fontWidth) {
				fbX = 0;
				fbY++;
			} else if (fbY >= fbHeight / fontHeight) {
				// TODO: Scroll.
			} else {
				renderChars(fbPointer, fbWidth,
						fbX, fbY, &c, 1, 15, -1,
						std::integral_constant<int, fontWidth>{},
						std::integral_constant<int, fontHeight>{});
				fbX++;
			}
		}
	}

	void print(const char *str) {
		while (*str)
			print(*str++);
	}
};

OutputSink infoSink;

void friggBeginLog() { }
void friggEndLog() { }

void friggPrintCritical(char c) {
	infoSink.print(c);
}

void friggPrintCritical(const char *str) {
	infoSink.print(str);
}

void friggPanic() {
	while(true) { }
	__builtin_unreachable();
}

extern "C" void eirRaspi4Main(uintptr_t deviceTreePtr) {
	// the device tree pointer is 32-bit and the upper bits are undefined
	deviceTreePtr &= 0x00000000FFFFFFFF;

	// FIXME: delay to slow the code down enough so we don't change the resolution
	// while the QEMU window didn't open yet (avoid a crash in framebuffer_update_display)
	for (size_t i = 0; i < 1000000; i++)
		asm volatile ("" ::: "memory");

	infoSink.init();

	BcmFBInfo fb{};
	fb.width = fb.vwidth = 1024;
	fb.height = fb.vheight = 768;
	fb.depth = 32;
	fb.x = fb.y = fb.pitch = fb.pointer = fb.size = 0;

	asm volatile ("dsb st; dmb st; isb" ::: "memory");
	eirMboxWrite(MboxChannel::fb, reinterpret_cast<uintptr_t>(&fb));
	asm volatile ("dsb st; dmb st; isb" ::: "memory");
	auto r = eirMboxRead(MboxChannel::fb);
	asm volatile ("dsb st; dmb st; isb" ::: "memory");

	if (r) {
		frigg::infoLogger() << "Framebuffer setup failed" << frigg::endLog;
	} else {
		infoSink.initFb(fb);
		frigg::infoLogger() << "Framebuffer info:" << frigg::endLog;
		frigg::infoLogger() << "Framebuffer pointer: " << reinterpret_cast<void *>(fb.pointer) << frigg::endLog;
		frigg::infoLogger() << "Framebuffer pitch: " << fb.pitch << frigg::endLog;
		frigg::infoLogger() << "Framebuffer width: " << fb.width << frigg::endLog;
		frigg::infoLogger() << "Framebuffer height: " << fb.height << frigg::endLog;
		frigg::infoLogger() << "Framebuffer depth: " << fb.depth << frigg::endLog;
	}

	auto dtb = reinterpret_cast<void *>(deviceTreePtr);

	frigg::infoLogger() << "Starting Eir" << frigg::endLog;
	frigg::infoLogger() << "DTB pointer " << dtb << frigg::endLog;

	auto initrd = reinterpret_cast<void *>(0x8000000);

	frigg::infoLogger() << "Assuming initrd is at " << initrd << frigg::endLog;

	CpioRange range{initrd};
	frigg::infoLogger() << "Assuming initrd ends at " << range.eof() << frigg::endLog;

	for (auto entry : range) {
		frigg::StringView sv{entry.name.data(), entry.name.size()};
		frigg::infoLogger() << "Got initrd entry: " << sv << frigg::endLog;
	}

	while(true);
}
