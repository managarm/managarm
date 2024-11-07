#include <stdint.h>
#include <assert.h>
#include <eir-internal/debug.hpp>
#include <eir/interface.hpp>
#include <render-text.hpp>
#include <dtb.hpp>
#include <frg/eternal.hpp> // for aligned_storage
#include <frg/manual_box.hpp>
#include <frg/tuple.hpp>
#include <eir-internal/arch/pl011.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/generic.hpp>

#include <arch/aarch64/mem_space.hpp>
#include <arch/register.hpp>

#include <arch/bit.hpp>
#include <arch/variable.hpp>

//#define RASPI3
//#define LOW_PERIPH

namespace eir {

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

		// Alt 0
		space.store(reg::sel1, space.load(reg::sel1) / sel1_p14(4) / sel1_p15(4));
		// No pull up/down
		space.store(reg::pup_pdn0, space.load(reg::pup_pdn0) / pup_pdn0_p14(0) / pup_pdn0_p15(0));
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
		while(space.load(reg::status) & status::full)
			;

		space.store(reg::write, io::channel(channel) | io::value(value >> 4));
	}

	uint32_t read(Channel channel) {
		while(space.load(reg::status) & status::empty)
			;

		auto f = space.load(reg::read);

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

	frg::tuple<int, int, void *, size_t> setupFb(unsigned int width, unsigned int height, unsigned int bpp) {
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
		*ptr++ = 0; // RGB

		*ptr++ = 0x00040001; // Allocate buffer
		*ptr++ = 8;
		*ptr++ = 0;
		*ptr++ = 0x1000;
		*ptr++ = 0;

		*ptr++ = 0x00040008; // Get pitch
		*ptr++ = 4;
		*ptr++ = 0;
		*ptr++ = 0;

		*ptr++ = 0;

		*ptr++ = 0x00000000;

		auto val = reinterpret_cast<uint64_t>(stor.buffer);
		assert(!(val & ~(uint64_t(0xFFFFFFF0))));
		Mbox::write(Mbox::Channel::property, val);

		auto ret = Mbox::read(Mbox::Channel::property);
		assert(val == ret);

		ptr = reinterpret_cast<volatile uint32_t *>(ret);

		auto fbPtr = 0;

		// if depth is not the expected depth, pretend we failed
		if(ptr[20] == bpp) { // depth == expected depth
#ifndef RASPI3
			// Translate legacy master view address into our address space
			fbPtr = ptr[28] - 0xC0000000;
#else
			fbPtr = ptr[28];
#endif
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

frg::manual_box<PL011> debugUart;

void debugPrintChar(char c) {
	debugUart->send(c);
}

extern "C" [[noreturn]] void eirRaspi4Main(uintptr_t deviceTreePtr) {
	// the device tree pointer is 32-bit and the upper bits are undefined
	deviceTreePtr &= 0x00000000FFFFFFFF;

	debugUart.initialize(mmioBase + 0x201000, 4000000);
	debugUart->disable();
	Gpio::configUart0Gpio();
	PropertyMbox::setClockFreq(PropertyMbox::Clock::uart, 4000000);
	debugUart->init(115200);

	char cmd_buf[1024];
	size_t cmd_len = PropertyMbox::getCmdline<1024>(cmd_buf);

	frg::string_view cmd_sv{cmd_buf, cmd_len};
	eir::infoLogger() << "Got cmdline: " << cmd_sv << frg::endlog;

	eir::infoLogger() << "Attempting to set up a framebuffer:" << frg::endlog;
	unsigned int fb_width = 0, fb_height = 0;

	// Parse the command line.
	{
		const char *l = cmd_buf;
		while(true) {
			while(*l && *l == ' ')
				l++;
			if(!(*l))
				break;

			const char *s = l;
			while(*s && *s != ' ')
				s++;

			frg::string_view token{l, static_cast<size_t>(s - l)};

			if(auto equals = token.find_first('='); equals != size_t(-1)) {
				auto key = token.sub_string(0, equals);
				auto value = token.sub_string(equals + 1, token.size() - equals - 1);

				if(key == "bcm2708_fb.fbwidth") {
					if(auto width = value.to_number<unsigned int>(); width)
						fb_width = *width;
				} else if(key == "bcm2708_fb.fbheight") {
					if(auto height = value.to_number<unsigned int>(); height)
						fb_height = *height;
				}
			}

			l = s;
		}
	}

	uintptr_t fb_ptr = 0;
	size_t fb_pitch = 0;
	bool have_fb = false;
	if(!fb_width || !fb_height) {
		eir::infoLogger() << "No display attached" << frg::endlog;
	} else {
		auto [actualW, actualH, ptr, pitch] = PropertyMbox::setupFb(fb_width, fb_height, 32);

		if(!ptr || !pitch) {
			eir::infoLogger() << "Mode setting failed..." << frg::endlog;
		} else {
			eir::infoLogger() << "Success!" << frg::endlog;
			setFbInfo(ptr, actualW, actualH, pitch);
			fb_ptr = reinterpret_cast<uintptr_t>(ptr);
			fb_width = actualW;
			fb_height = actualH;
			fb_pitch = pitch;
			have_fb = true;
			eir::infoLogger() << "Framebuffer pointer: " << ptr << frg::endlog;
			eir::infoLogger() << "Framebuffer pitch: " << pitch << frg::endlog;
			eir::infoLogger() << "Framebuffer width: " << actualW << frg::endlog;
			eir::infoLogger() << "Framebuffer height: " << actualH << frg::endlog;
		}
	}

	GenericInfo info{
		.deviceTreePtr = deviceTreePtr,
		.cmdline = cmd_buf,
		.fb {},
		.debugFlags = eirDebugSerial,
		.hasFb = have_fb
	};

	if(have_fb) {
		info.fb = {
			.fbAddress = fb_ptr,
			.fbPitch = fb_pitch,
			.fbWidth = static_cast<EirSize>(fb_width),
			.fbHeight = static_cast<EirSize>(fb_height),
			.fbBpp = 32,
			.fbType = 0
		};
	}

	eirGenericMain(info);
}

} // namespace eir
