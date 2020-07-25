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
#include <frg/eternal.hpp> // for aligned_storage
#include <frg/tuple.hpp>

#include <arch/aarch64/mem_space.hpp>
#include <arch/register.hpp>

//#define RASPI3
//#define LOW_PERIPH

#if defined(RASPI3)
static constexpr inline uintptr_t mmioBase = 0x3f000000;
#elif defined (LOW_PERIPH)
static constexpr inline uintptr_t mmioBase = 0xfe000000;
#else
static constexpr inline uintptr_t mmioBase = 0x47e000000;
#endif

namespace Gpio {
	namespace reg {
		static constexpr arch::bit_register<uint32_t> sel1{0x04};
		static constexpr arch::bit_register<uint32_t> pup_pdn0{0xE4};
	}

	static constexpr arch::mem_space space{mmioBase + 0x200000};

	void configUart0Gpio() {
		arch::field<uint32_t, uint8_t> sel1_p14{12, 3};
		arch::field<uint32_t, uint8_t> sel1_p15{15, 3};

		arch::field<uint32_t, uint8_t> pup_pdn0_p14{28, 2};
		arch::field<uint32_t, uint8_t> pup_pdn0_p15{30, 2};

		asm volatile ("dsb st; dmb st; isb" ::: "memory");
		// Alt 0
		space.store(reg::sel1, space.load(reg::sel1) / sel1_p14(4) / sel1_p15(4));
		// No pull up/down
		space.store(reg::pup_pdn0, space.load(reg::pup_pdn0) / pup_pdn0_p14(0) / pup_pdn0_p15(0));
		asm volatile ("dsb st; dmb st; isb" ::: "memory");
	}
}

namespace Mbox {
	static constexpr arch::mem_space space{mmioBase + 0xb880};

	namespace reg {
		static constexpr arch::bit_register<uint32_t> read{0x00};
		static constexpr arch::bit_register<uint32_t> status{0x18};
		static constexpr arch::bit_register<uint32_t> write{0x20};
	}

	enum class Channel {
		pmi = 0,
		fb,
		vuart,
		vchiq,
		led,
		button,
		touch,
		property = 8
	};

	namespace io {
		static constexpr arch::field<uint32_t, Channel> channel{0, 4};
		static constexpr arch::field<uint32_t, uint32_t> value{4, 28};
	}

	namespace status {
		static constexpr arch::field<uint32_t, bool> empty{30, 1};
		static constexpr arch::field<uint32_t, bool> full{31, 1};
	}

	void write(Channel channel, uint32_t value) {
		asm volatile ("dsb st; dmb st; isb" ::: "memory");
		while (space.load(reg::status) & status::full)
			;

		space.store(reg::write, io::channel(channel) | io::value(value >> 4));
		asm volatile ("dsb st; dmb st; isb" ::: "memory");
	}

	uint32_t read(Channel channel) {
		asm volatile ("dsb st; dmb st; isb" ::: "memory");
		while (space.load(reg::status) & status::empty)
			;

		auto f = space.load(reg::read);

		asm volatile ("dsb st; dmb st; isb" ::: "memory");
		return (f & io::value) << 4;
	}
}

namespace PropertyMbox {
	enum class Clock {
		uart = 2
	};

	void setClockFreq(Clock clock, uint32_t freq, bool turbo = false) {
		constexpr uint32_t req_size = 9 * 4;
		frg::aligned_storage<req_size, 16> stor;
		auto ptr = reinterpret_cast<volatile uint32_t *>(stor.buffer);

		*ptr++ = req_size;
		*ptr++ = 0x00000000; // Process request
		*ptr++ = 0x00038002; // Set clock rate
		*ptr++ = 12;
		*ptr++ = 8;
		*ptr++ = static_cast<uint32_t>(clock);
		*ptr++ = freq;
		*ptr++ = turbo;
		*ptr++ = 0x00000000;

		auto val = reinterpret_cast<uint64_t>(stor.buffer);
		assert(!(val & ~(uint64_t(0xFFFFFFF0))));
		Mbox::write(Mbox::Channel::property, val);

		auto ret = Mbox::read(Mbox::Channel::property);
		assert(val == ret);
	}

	frg::tuple<int, int, void *, size_t> setupFb(int width, int height, int bpp) {
		constexpr uint32_t req_size = 36 * 4;
		frg::aligned_storage<req_size, 16> stor;
		auto ptr = reinterpret_cast<volatile uint32_t *>(stor.buffer);

		*ptr++ = req_size;
		*ptr++ = 0x00000000; // Process request

		*ptr++ = 0x00048003; // Set physical width/height
		*ptr++ = 8;
		*ptr++ = 0;
		*ptr++ = width;
		*ptr++ = height;

		*ptr++ = 0x00048004; // Set virtual width/height
		*ptr++ = 8;
		*ptr++ = 0;
		*ptr++ = width;
		*ptr++ = height;

		*ptr++ = 0x00048009; // Set virtual offset
		*ptr++ = 8;
		*ptr++ = 0;
		*ptr++ = 0;
		*ptr++ = 0;

		*ptr++ = 0x00048005; // Set depth
		*ptr++ = 4;
		*ptr++ = 0;
		*ptr++ = bpp;

		*ptr++ = 0x00048006; // Set pixel order
		*ptr++ = 4;
		*ptr++ = 0;
		*ptr++ = 1; // RGB

		*ptr++ = 0x00040001; // Allocate buffer
		*ptr++ = 8;
		*ptr++ = 0;
		*ptr++ = 16;
		*ptr++ = 0;

		*ptr++ = 0x00040008; // Get pitch
		*ptr++ = 4;
		*ptr++ = 0;
		*ptr++ = 0;

		*ptr++ = 0;

		*ptr++ = 0x00000000;
		asm volatile ("dsb st; dmb st; isb" ::: "memory");

		auto val = reinterpret_cast<uint64_t>(stor.buffer);
		assert(!(val & ~(uint64_t(0xFFFFFFF0))));
		Mbox::write(Mbox::Channel::property, val);

		auto ret = Mbox::read(Mbox::Channel::property);
		assert(val == ret);

		ptr = reinterpret_cast<volatile uint32_t *>(ret);

		auto fbPtr = 0;

		// if depth is not the expected depth, pretend we failed
		if (ptr[20] == bpp) { // depth == expected depth
			// Translate legacy master view address into our address space
			fbPtr = ptr[28] - 0xC0000000;
		}

		return frg::make_tuple(int(ptr[5]), int(ptr[6]), reinterpret_cast<void *>(fbPtr), size_t(ptr[33]));
	}

	template <size_t MaxSize>
	size_t getCmdline(void *dest) requires (!(MaxSize & 3)) {
		constexpr uint32_t req_size = 5 * 4 + MaxSize;
		frg::aligned_storage<req_size, 16> stor;
		memset(stor.buffer, 0, req_size);

		auto ptr = reinterpret_cast<volatile uint32_t *>(stor.buffer);

		*ptr++ = req_size;
		*ptr++ = 0x00000000; // Process request

		*ptr++ = 0x00050001; // Get commandline
		*ptr++ = MaxSize;

		asm volatile ("dsb st; dmb st; isb" ::: "memory");

		auto val = reinterpret_cast<uint64_t>(stor.buffer);
		assert(!(val & ~(uint64_t(0xFFFFFFF0))));
		Mbox::write(Mbox::Channel::property, val);

		auto ret = Mbox::read(Mbox::Channel::property);
		assert(val == ret);

		ptr = reinterpret_cast<volatile uint32_t *>(ret);

		auto data = reinterpret_cast<char *>(ret + 20);
		auto totalLen = ptr[3];
		auto cmdlineLen = strlen(data);

		assert(totalLen <= MaxSize);
		memcpy(dest, data, cmdlineLen + 1);

		return cmdlineLen;
	}
}

namespace PL011 {
	namespace reg {
		static constexpr arch::scalar_register<uint32_t> data{0x00};
		static constexpr arch::bit_register<uint32_t> status{0x18};
		static constexpr arch::scalar_register<uint32_t> i_baud{0x24};
		static constexpr arch::scalar_register<uint32_t> f_baud{0x28};
		static constexpr arch::bit_register<uint32_t> control{0x30};
		static constexpr arch::bit_register<uint32_t> line_control{0x2c};
		static constexpr arch::scalar_register<uint32_t> int_clear{0x44};
	}

	namespace status {
		static constexpr arch::field<uint32_t, bool> tx_full{5, 1};
	};

	namespace control {
		static constexpr arch::field<uint32_t, bool> rx_en{9, 1};
		static constexpr arch::field<uint32_t, bool> tx_en{8, 1};
		static constexpr arch::field<uint32_t, bool> uart_en{0, 1};
	};

	namespace line_control {
		static constexpr arch::field<uint32_t, uint8_t> word_len{5, 2};
		static constexpr arch::field<uint32_t, bool> fifo_en{4, 1};
	}

	static constexpr arch::mem_space space{mmioBase + 0x201000};
	constexpr uint64_t clock = 4000000; // 4MHz

	void init(uint64_t baud) {
		asm volatile ("dsb st; dmb st; isb" ::: "memory");
		space.store(reg::control, control::uart_en(false));

		Gpio::configUart0Gpio();

		space.store(reg::int_clear, 0x7FF); // clear all interrupts

		PropertyMbox::setClockFreq(PropertyMbox::Clock::uart, clock);

		uint64_t int_part = clock / (16 * baud);

		// 3 decimal places of precision should be enough :^)
		uint64_t frac_part = (((clock * 1000) / (16 * baud) - (int_part * 1000))
			* 64 + 500) / 1000;

		space.store(reg::i_baud, int_part);
		space.store(reg::f_baud, frac_part);

		// 8n1, fifo enabled
		space.store(reg::line_control, line_control::word_len(3) | line_control::fifo_en(true));
		space.store(reg::control, control::rx_en(true) | control::tx_en(true) | control::uart_en(true));
		asm volatile ("dsb st; dmb st; isb" ::: "memory");
	}

	void send(uint8_t val) {
		asm volatile ("dsb st; dmb st; isb" ::: "memory");
		while (space.load(reg::status) & status::tx_full)
			;

		space.store(reg::data, val);
		asm volatile ("dsb st; dmb st; isb" ::: "memory");
	}
}

struct OutputSink {
	uint32_t fbWidth;
	uint32_t fbHeight;
	uint32_t fbPitch;
	void *fbPointer;

	uint32_t fbX;
	uint32_t fbY;

	static constexpr uint32_t fontWidth = 8;
	static constexpr uint32_t fontHeight = 16;

	void init() {
		PL011::init(115200);
		fbPointer = nullptr;
	}

	void initFb(int width, int height, int pitch, void *ptr) {
		fbWidth = width;
		fbHeight = height;
		fbPitch = pitch;
		fbPointer = ptr;

		fbX = 0;
		fbY = 0;
	}

	void print(char c) {
		PL011::send(c);

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

	// TODO: actually get display size from cmdline
	frigg::infoLogger() << "Attempting to get the display size" << frigg::endLog;
	int width = 1920, height = 1080;
	if (!width || !height) {
		frigg::infoLogger() << "Zero fb width or height, no display attached?" << frigg::endLog;
	} else {
		frigg::infoLogger() << "Attempting to set up the framebuffer" << frigg::endLog;
		auto [actualW, actualH, ptr, pitch] = PropertyMbox::setupFb(width, height, 32);

		if (!ptr || !pitch) {
			frigg::infoLogger() << "Mode setting failed..." << frigg::endLog;
		} else {
			infoSink.initFb(actualW, actualH, pitch, ptr);
			frigg::infoLogger() << "Framebuffer pointer: " << ptr << frigg::endLog;
			frigg::infoLogger() << "Framebuffer pitch: " << pitch << frigg::endLog;
			frigg::infoLogger() << "Framebuffer width: " << actualW << frigg::endLog;
			frigg::infoLogger() << "Framebuffer height: " << actualH << frigg::endLog;

		}
	}

	char buf[1024];
	size_t len = PropertyMbox::getCmdline<1024>(buf);

	frigg::StringView cmd_sv{buf, len};
	frigg::infoLogger() << "Got cmdline: " << cmd_sv << frigg::endLog;

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
