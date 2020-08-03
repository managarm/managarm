#include <stdint.h>
#include <frigg/c-support.h>
#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>
#include <eir-internal/debug.hpp>
#include <frigg/support.hpp>
#include <frigg/string.hpp>
#include <eir/interface.hpp>
#include <frigg/libc.hpp>
#include <render-text.hpp>
#include "../cpio.hpp"
#include <frg/eternal.hpp> // for aligned_storage
#include <frg/tuple.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/generic.hpp>

#include <arch/aarch64/mem_space.hpp>
#include <arch/register.hpp>

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

void debugPrintChar(char c) {
	PL011::send(c);
}

extern "C" void eirRaspi4Main(uintptr_t deviceTreePtr) {
	// the device tree pointer is 32-bit and the upper bits are undefined
	deviceTreePtr &= 0x00000000FFFFFFFF;

	// FIXME: delay to slow the code down enough so we don't change the resolution
	// while the QEMU window didn't open yet (avoid a crash in framebuffer_update_display)
	for (size_t i = 0; i < 10000000; i++)
		asm volatile ("" ::: "memory");

	PL011::init(115200);

	// TODO: actually get display size from cmdline
	eir::infoLogger() << "Attempting to get the display size" << frg::endlog;
	int width = 1920, height = 1080;
	if (!width || !height) {
		eir::infoLogger() << "Zero fb width or height, no display attached?" << frg::endlog;
	} else {
		eir::infoLogger() << "Attempting to set up the framebuffer" << frg::endlog;
		auto [actualW, actualH, ptr, pitch] = PropertyMbox::setupFb(width, height, 32);

		if (!ptr || !pitch) {
			eir::infoLogger() << "Mode setting failed..." << frg::endlog;
		} else {
			setFbInfo(ptr, actualW, actualH, pitch);
			eir::infoLogger() << "Framebuffer pointer: " << ptr << frg::endlog;
			eir::infoLogger() << "Framebuffer pitch: " << pitch << frg::endlog;
			eir::infoLogger() << "Framebuffer width: " << actualW << frg::endlog;
			eir::infoLogger() << "Framebuffer height: " << actualH << frg::endlog;
		}
	}

	char buf[1024];
	size_t len = PropertyMbox::getCmdline<1024>(buf);

	frg::string_view cmd_sv{buf, len};
	eir::infoLogger() << "Got cmdline: " << cmd_sv << frg::endlog;

	auto dtb = reinterpret_cast<void *>(deviceTreePtr);

	initProcessorEarly();
	eir::infoLogger() << "DTB pointer " << dtb << frg::endlog;

	auto initrd = reinterpret_cast<void *>(0x8000000);

	eir::infoLogger() << "Assuming initrd is at " << initrd << frg::endlog;

	CpioRange range{initrd};
	eir::infoLogger() << "Assuming initrd ends at " << range.eof() << frg::endlog;

	for (auto entry : range) {
		eir::infoLogger() << "Got initrd entry: " << entry.name << frg::endlog;
	}

	while(true);
}

enum class IntrType {
	synchronous,
	irq,
	fiq,
	serror
};

extern "C" void eirExceptionHandler(IntrType i_type, uintptr_t syndrome, uintptr_t link,
		uintptr_t state, uintptr_t fault_addr) {

	// Disable MMU to gain back the ability to use the screen and uart
	uint64_t sctlr;
	asm volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));

	sctlr &= ~1;

	asm volatile ("msr sctlr_el1, %0" :: "r"(sctlr));

	eir::infoLogger() << "An unexpected fault has occured:" << frg::endlog;

	const char *i_type_str = "Unknown";
	switch (i_type) {
		case IntrType::synchronous:
			i_type_str = "synchronous";
			break;
		case IntrType::irq:
			i_type_str = "irq";
			break;
		case IntrType::fiq:
			i_type_str = "fiq";
			break;
		case IntrType::serror:
			i_type_str = "SError";
			break;
	}

	eir::infoLogger() << "Interruption type: " << i_type_str << frg::endlog;

	auto exc_type = syndrome >> 26;
	const char *exc_type_str = "Unknown";
	switch (exc_type) {
		case 0x01: exc_type_str = "Trapped WFI/WFE"; break;
		case 0x0e: exc_type_str = "Illegal execution"; break;
		case 0x15: exc_type_str = "System call"; break;
		case 0x20: exc_type_str = "Instruction abort, lower EL"; break;
		case 0x21: exc_type_str = "Instruction abort, same EL"; break;
		case 0x22: exc_type_str = "Instruction alignment fault"; break;
		case 0x24: exc_type_str = "Data abort, lower EL"; break;
		case 0x25: exc_type_str = "Data abort, same EL"; break;
		case 0x26: exc_type_str = "Stack alignment fault"; break;
		case 0x2c: exc_type_str = "Floating point"; break;
	}

	eir::infoLogger() << "Exception type: " << exc_type_str << " (" << (void *)exc_type << ")" << frg::endlog;

	auto iss = syndrome & ((1 << 25) - 1);

	if (exc_type == 0x25 || exc_type == 0x24) {
		constexpr const char *sas_values[4] = {"Byte", "Halfword", "Word", "Doubleword"};
		constexpr const char *set_values[4] = {"Recoverable", "Uncontainable", "Reserved", "Restartable/Corrected"};
		constexpr const char *dfsc_values[4] = {"Address size", "Translation", "Access flag", "Permission"};
		eir::infoLogger() << "Access size: " << sas_values[(iss >> 22) & 3] << frg::endlog;
		eir::infoLogger() << "Sign extended? " << (iss & (1 << 21) ? "Yes" : "No") << frg::endlog;
		eir::infoLogger() << "Sixty-Four? " << (iss & (1 << 15) ? "Yes" : "No") << frg::endlog;
		eir::infoLogger() << "Acquire/Release? " << (iss & (1 << 14) ? "Yes" : "No") << frg::endlog;
		eir::infoLogger() << "Synch error type: " << set_values[(iss >> 11) & 3] << frg::endlog;
		eir::infoLogger() << "Fault address valid? " << (iss & (1 << 10) ? "No" : "Yes") << frg::endlog;
		eir::infoLogger() << "Cache maintenance? " << (iss & (1 << 8) ? "Yes" : "No") << frg::endlog;
		eir::infoLogger() << "S1PTW? " << (iss & (1 << 7) ? "Yes" : "No") << frg::endlog;
		eir::infoLogger() << "Access type: " << (iss & (1 << 6) ? "Write" : "Read") << frg::endlog;
		if ((iss & 0b111111) <= 0b001111)
			eir::infoLogger() << "Data fault status code: " << dfsc_values[(iss >> 2) & 4] << " fault level " << (iss & 3) << frg::endlog;
		else if ((iss & 0b111111) == 0b10000)
			eir::infoLogger() << "Data fault status code: Synchronous external fault" << frg::endlog;
		else if ((iss & 0b111111) == 0b100001)
			eir::infoLogger() << "Data fault status code: Alignment fault" << frg::endlog;
		else if ((iss & 0b111111) == 0b110000)
			eir::infoLogger() << "Data fault status code: TLB conflict abort" << frg::endlog;
		else
			eir::infoLogger() << "Data fault status code: unknown" << frg::endlog;
	}

	eir::infoLogger() << "IP: " << (void *)link << ", State: " << (void *)state << frg::endlog;
	eir::infoLogger() << "Syndrome: " << (void *)syndrome << ", Fault address: " << (void *)fault_addr << frg::endlog;
	eir::infoLogger() << "Halting..." << frg::endlog;

	while(1)
		asm volatile ("wfi");
}

} // namespace eir
