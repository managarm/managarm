#include <stdint.h>
#include <assert.h>
#include <eir-internal/debug.hpp>
#include <eir/interface.hpp>
#include <render-text.hpp>
#include "../cpio.hpp"
#include <frg/eternal.hpp> // for aligned_storage
#include <frg/tuple.hpp>
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

	frg::tuple<size_t, size_t> getArmMemory() {
		constexpr uint32_t req_size = 8 * 4;
		frg::aligned_storage<req_size, 16> stor;
		auto ptr = reinterpret_cast<volatile uint32_t *>(stor.buffer);

		*ptr++ = req_size;
		*ptr++ = 0x00000000; // Process request

		*ptr++ = 0x00010005; // Set ARM memory
		*ptr++ = 8;
		*ptr++ = 0;
		*ptr++ = 0; // base
		*ptr++ = 0; // size

		*ptr++ = 0x00000000;
		asm volatile ("dsb st; dmb st; isb" ::: "memory");

		auto val = reinterpret_cast<uint64_t>(stor.buffer);
		assert(!(val & ~(uint64_t(0xFFFFFFF0))));
		Mbox::write(Mbox::Channel::property, val);

		auto ret = Mbox::read(Mbox::Channel::property);
		assert(val == ret);

		ptr = reinterpret_cast<volatile uint32_t *>(ret);

		return frg::make_tuple(size_t(ptr[5]), size_t(ptr[6]));
	}

	frg::tuple<size_t, size_t> getGpuMemory() {
		constexpr uint32_t req_size = 8 * 4;
		frg::aligned_storage<req_size, 16> stor;
		auto ptr = reinterpret_cast<volatile uint32_t *>(stor.buffer);

		*ptr++ = req_size;
		*ptr++ = 0x00000000; // Process request

		*ptr++ = 0x00010006; // Set VC memory
		*ptr++ = 8;
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

		return frg::make_tuple(size_t(ptr[5]), size_t(ptr[6]));
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

struct DtbHeader {
	arch::scalar_storage<uint32_t, arch::big_endian> magic;
	arch::scalar_storage<uint32_t, arch::big_endian> totalsize;
	arch::scalar_storage<uint32_t, arch::big_endian> off_dt_struct;
	arch::scalar_storage<uint32_t, arch::big_endian> off_dt_strings;
	arch::scalar_storage<uint32_t, arch::big_endian> off_mem_rsvmap;
	arch::scalar_storage<uint32_t, arch::big_endian> version;
	arch::scalar_storage<uint32_t, arch::big_endian> last_comp_version;
	arch::scalar_storage<uint32_t, arch::big_endian> boot_cpuid_phys;
	arch::scalar_storage<uint32_t, arch::big_endian> size_dt_strings;
	arch::scalar_storage<uint32_t, arch::big_endian> size_dt_struct;
};

struct DtbReserveEntry {
	arch::scalar_storage<uint64_t, arch::big_endian> address;
	arch::scalar_storage<uint64_t, arch::big_endian> size;
};

DtbHeader getDtbHeader(void *dtb) {
	DtbHeader hdr;
	memcpy(&hdr, dtb, sizeof(hdr));

	assert(hdr.magic.load() == 0xd00dfeed);

	return hdr;
}

extern "C" void eirEnterKernel(uintptr_t, uintptr_t, uint64_t, uint64_t, uintptr_t);

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
	int fb_width = 1920, fb_height = 1080;
	uintptr_t fb_ptr = 0;
	size_t fb_pitch = 0;
	bool have_fb = false;
	if (!fb_width || !fb_height) {
		eir::infoLogger() << "Zero fb width or height, no display attached?" << frg::endlog;
	} else {
		eir::infoLogger() << "Attempting to set up the framebuffer" << frg::endlog;
		auto [actualW, actualH, ptr, pitch] = PropertyMbox::setupFb(fb_width, fb_height, 32);

		if (!ptr || !pitch) {
			eir::infoLogger() << "Mode setting failed..." << frg::endlog;
		} else {
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

	char cmd_buf[1024];
	size_t cmd_len = PropertyMbox::getCmdline<1024>(cmd_buf);

	frg::string_view cmd_sv{cmd_buf, cmd_len};
	eir::infoLogger() << "Got cmdline: " << cmd_sv << frg::endlog;

	auto [ram_base, ram_size] = PropertyMbox::getArmMemory();
	auto [gpu_base, gpu_size] = PropertyMbox::getGpuMemory();

	eir::infoLogger() << "Memory allocated to the cpu is at 0x" << frg::hex_fmt{ram_base}
			<< " - 0x" << frg::hex_fmt{ram_base + ram_size} << " (0x"
			<< frg::hex_fmt{ram_size} << " bytes)" << frg::endlog;

	eir::infoLogger() << "Memory allocated to the gpu is at 0x" << frg::hex_fmt{gpu_base}
			<< " - 0x" << frg::hex_fmt{gpu_base + gpu_size} << " (0x"
			<< frg::hex_fmt{gpu_size} << " bytes)" << frg::endlog;

	eir::infoLogger() << "Note: cpu memory size figure above is inaccurate" << frg::endlog;

	auto dtb = reinterpret_cast<void *>(deviceTreePtr);

	initProcessorEarly();

	bootMemoryLimit = reinterpret_cast<uintptr_t>(&eirImageCeiling);

	eir::infoLogger() << "DTB pointer " << dtb << frg::endlog;

	auto dtb_hdr = getDtbHeader(dtb);
	auto dtb_end = reinterpret_cast<uintptr_t>(dtb) + dtb_hdr.totalsize.load();

	eir::infoLogger() << "DTB size: 0x" << frg::hex_fmt{dtb_hdr.totalsize.load()} << frg::endlog;

	uintptr_t dtb_rsv = reinterpret_cast<uintptr_t>(dtb) + dtb_hdr.off_mem_rsvmap.load();

	eir::infoLogger() << "Memory reservation entries:" << frg::endlog;
	while (true) {
		DtbReserveEntry ent;
		memcpy(&ent, reinterpret_cast<void *>(dtb_rsv), 16);
		dtb_rsv += 16;

		if (!ent.address.load() && !ent.size.load())
			break;

		eir::infoLogger() << "At 0x" << frg::hex_fmt{ent.address.load()}
			<< ", ends at 0x" << frg::hex_fmt{ent.address.load() + ent.size.load()}
			<< " (0x" << frg::hex_fmt{ent.size.load()} << " bytes)" << frg::endlog;

		if (auto end = ent.address.load() + ent.size.load(); end > bootMemoryLimit)
			bootMemoryLimit = end;
	}

	bootMemoryLimit = (bootMemoryLimit + address_t(pageSize - 1)) & ~address_t(pageSize - 1);

	// TODO: parse DTB to get memory regions instead of trusing the firmware property mailbox call

	auto initrd = reinterpret_cast<void *>(0x8000000);
	eir::infoLogger() << "Assuming initrd is at " << initrd << frg::endlog;
	CpioRange cpio_range{initrd};
	auto end_initrd = cpio_range.eof();
	eir::infoLogger() << "Assuming initrd ends at " << end_initrd << frg::endlog;

	auto free_space = 0x8000000 - bootMemoryLimit;

	createInitialRegion(bootMemoryLimit, free_space & ~uintptr_t(0xFFF));
	free_space = reinterpret_cast<uintptr_t>(dtb) - reinterpret_cast<uintptr_t>(end_initrd);
	createInitialRegion((reinterpret_cast<uintptr_t>(end_initrd) + 0xFFF)
				& ~uintptr_t(0xFFF), free_space & ~uintptr_t(0xFFF));

	dtb_end = (dtb_end + address_t(pageSize - 1)) & ~address_t(pageSize - 1);
	free_space = ram_base + ram_size - dtb_end;
	createInitialRegion(dtb_end, free_space & ~uintptr_t(0xFFF));

	setupRegionStructs();

	eir::infoLogger() << "Kernel memory regions:" << frg::endlog;
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType == RegionType::null)
			continue;
		eir::infoLogger() << "    Memory region [" << i << "]."
				<< " Base: 0x" << frg::hex_fmt{regions[i].address}
				<< ", length: 0x" << frg::hex_fmt{regions[i].size} << frg::endlog;
		if(regions[i].regionType == RegionType::allocatable)
			eir::infoLogger() << "        Buddy tree at 0x" << frg::hex_fmt{regions[i].buddyTree}
					<< ", overhead: 0x" << frg::hex_fmt{regions[i].buddyOverhead}
					<< frg::endlog;
	}

	frg::span<uint8_t> kernel_image{nullptr, 0};

	for (auto entry : cpio_range) {
		if (entry.name == "thor") {
			kernel_image = entry.data;
		}
	}

	assert(kernel_image.data() && kernel_image.size());

	uint64_t kernel_entry = 0;
	initProcessorPaging(kernel_image.data(), kernel_entry);

	auto info_ptr = generateInfo(cmd_buf);

	auto module = bootAlloc<EirModule>();
	module->physicalBase = reinterpret_cast<uintptr_t>(initrd);
	module->length = reinterpret_cast<uintptr_t>(end_initrd)
			- reinterpret_cast<uintptr_t>(initrd);

	char *name_ptr = bootAlloc<char>(11);
	memcpy(name_ptr, "initrd.cpio", 11);
	module->namePtr = mapBootstrapData(name_ptr);
	module->nameLength = 11;

	info_ptr->numModules = 1;
	info_ptr->moduleInfo = mapBootstrapData(module);

	if (have_fb) {
		auto framebuf = &info_ptr->frameBuffer;
		framebuf->fbAddress = fb_ptr;
		framebuf->fbPitch = fb_pitch;
		framebuf->fbWidth = fb_width;
		framebuf->fbHeight = fb_height;
		framebuf->fbBpp = 32;
		framebuf->fbType = 0;

		assert(fb_ptr & ~(pageSize - 1));
		for(address_t pg = 0; pg < fb_pitch * fb_height; pg += 0x1000)
			mapSingle4kPage(0xFFFF'FE00'4000'0000 + pg, fb_ptr + pg,
					PageFlags::write, CachingMode::writeCombine);
		mapKasanShadow(0xFFFF'FE00'4000'0000, fb_pitch * fb_height);
		unpoisonKasanShadow(0xFFFF'FE00'4000'0000, fb_pitch * fb_height);
		framebuf->fbEarlyWindow = 0xFFFF'FE00'4000'0000;
	}

	info_ptr->debugFlags |= eirDebugSerial;

	mapSingle4kPage(0xFFFF'0000'0000'0000, mmioBase + 0x201000,
			PageFlags::write, CachingMode::mmio);
	mapKasanShadow(0xFFFF'0000'0000'0000, 0x1000);
	unpoisonKasanShadow(0xFFFF'0000'0000'0000, 0x1000);

	eir::infoLogger() << "Leaving Eir and entering the real kernel" << frg::endlog;

	eirEnterKernel(eirTTBR[0] + 1, eirTTBR[1] + 1, kernel_entry,
			0xFFFF'FE80'0001'0000, reinterpret_cast<uintptr_t>(info_ptr));

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
