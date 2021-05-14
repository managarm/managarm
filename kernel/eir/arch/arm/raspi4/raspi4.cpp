#include <stdint.h>
#include <assert.h>
#include <eir-internal/debug.hpp>
#include <eir/interface.hpp>
#include <render-text.hpp>
#include <dtb.hpp>
#include "../cpio.hpp"
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
		while (space.load(reg::status) & status::full)
			;

		space.store(reg::write, io::channel(channel) | io::value(value >> 4));
	}

	uint32_t read(Channel channel) {
		while (space.load(reg::status) & status::empty)
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

extern "C" void eirRaspi4Main(uintptr_t deviceTreePtr) {
	// the device tree pointer is 32-bit and the upper bits are undefined
	deviceTreePtr &= 0x00000000FFFFFFFF;

	// FIXME: delay to slow the code down enough so we don't change the resolution
	// while the QEMU window didn't open yet (avoid a crash in framebuffer_update_display)
	for (size_t i = 0; i < 10000000; i++)
		asm volatile ("" ::: "memory");

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
	int fb_width = 0, fb_height = 0;

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

			if (auto equals = token.find_first('='); equals != size_t(-1)) {
				auto key = token.sub_string(0, equals);
				auto value = token.sub_string(equals + 1, token.size() - equals - 1);

				if (key == "bcm2708_fb.fbwidth") {
					if (auto width = value.to_number<int>(); width)
						fb_width = *width;
				} else if (key == "bcm2708_fb.fbheight") {
					if (auto height = value.to_number<int>(); height)
						fb_height = *height;
				}
			}

			l = s;
		}
	}

	uintptr_t fb_ptr = 0;
	size_t fb_pitch = 0;
	bool have_fb = false;
	if (!fb_width || !fb_height) {
		eir::infoLogger() << "No display attached" << frg::endlog;
	} else {
		auto [actualW, actualH, ptr, pitch] = PropertyMbox::setupFb(fb_width, fb_height, 32);

		if (!ptr || !pitch) {
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

	initProcessorEarly();

	DeviceTree dt{reinterpret_cast<void *>(deviceTreePtr)};

	eir::infoLogger() << "DTB pointer " << dt.data() << frg::endlog;
	eir::infoLogger() << "DTB size: 0x" << frg::hex_fmt{dt.size()} << frg::endlog;

	DeviceTreeNode chosenNode;
	bool hasChosenNode = false;

	DeviceTreeNode memoryNodes[32];
	size_t nMemoryNodes = 0;

	dt.rootNode().discoverSubnodes(
		[](DeviceTreeNode &node) {
			return !memcmp("memory@", node.name(), 7)
				|| !memcmp("chosen", node.name(), 7);
		},
		[&](DeviceTreeNode node) {
			if (!memcmp("chosen", node.name(), 7)) {
				assert(!hasChosenNode);

				chosenNode = node;
				hasChosenNode = true;
			} else {
				assert(nMemoryNodes < 32);

				memoryNodes[nMemoryNodes++] = node;
			}
			infoLogger() << "Node \"" << node.name() << "\" discovered" << frg::endlog;
		});

	uint32_t addressCells = 2, sizeCells = 1;

	for (auto prop : dt.rootNode().properties()) {
		if (!memcmp("#address-cells", prop.name(), 15)) {
			addressCells = prop.asU32();
		} else if (!memcmp("#size-cells", prop.name(), 12)) {
			sizeCells = prop.asU32();
		}
	}

	assert(nMemoryNodes && hasChosenNode);

	InitialRegion reservedRegions[32];
	size_t nReservedRegions = 0;

	eir::infoLogger() << "Memory reservation entries:" << frg::endlog;
	for (auto ent : dt.memoryReservations()) {
		eir::infoLogger() << "At 0x" << frg::hex_fmt{ent.address}
			<< ", ends at 0x" << frg::hex_fmt{ent.address + ent.size}
			<< " (0x" << frg::hex_fmt{ent.size} << " bytes)" << frg::endlog;

		reservedRegions[nReservedRegions++] = {ent.address, ent.size};
	}
	eir::infoLogger() << "End of memory reservation entries" << frg::endlog;

	uintptr_t eirStart = reinterpret_cast<uintptr_t>(&eirImageFloor);
	uintptr_t eirEnd = reinterpret_cast<uintptr_t>(&eirImageCeiling);
	reservedRegions[nReservedRegions++] = {eirStart, eirEnd - eirStart};

	uintptr_t initrd = 0;
	if (auto p = chosenNode.findProperty("linux,initrd-start"); p) {
		switch (p->size()) {
		case 4:	initrd = p->asU32(); break;
		case 8:	initrd = p->asU64(); break;
		default: assert(!"Invalid linux,initrd-start size");
		}

		eir::infoLogger() << "Initrd is at " << (void *)initrd << frg::endlog;
	} else {
		initrd = 0x8000000;
		eir::infoLogger() << "Assuming initrd is at " << (void *)initrd << frg::endlog;
	}

	CpioRange cpio_range{reinterpret_cast<void *>(initrd)};

	auto initrd_end = reinterpret_cast<uintptr_t>(cpio_range.eof());
	eir::infoLogger() << "Initrd ends at " << (void *)initrd_end << frg::endlog;

	reservedRegions[nReservedRegions++] = {initrd, initrd_end - initrd};
	reservedRegions[nReservedRegions++] = {deviceTreePtr, dt.size()};

	for (int i = 0; i < nMemoryNodes; i++) {
		auto reg = memoryNodes[i].findProperty("reg");
		assert(reg);

		size_t j = 0;
		while (j < reg->size()) {
			auto base = reg->asPropArrayEntry(addressCells, j);
			j += addressCells * 4;

			auto size = reg->asPropArrayEntry(sizeCells, j);
			j += sizeCells * 4;

			createInitialRegions({base, size}, {reservedRegions, nReservedRegions});
		}
	}

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
	module->physicalBase = initrd;
	module->length = initrd_end - initrd;

	char *name_ptr = bootAlloc<char>(11);
	memcpy(name_ptr, "initrd.cpio", 11);
	module->namePtr = mapBootstrapData(name_ptr);
	module->nameLength = 11;

	info_ptr->numModules = 1;
	info_ptr->moduleInfo = mapBootstrapData(module);

	info_ptr->dtbPtr = deviceTreePtr;
	info_ptr->dtbSize = dt.size();

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
			0xFFFF'FE80'0001'0000, 0xFFFF'FE80'0001'0000);

	while(true);
}

} // namespace eir
