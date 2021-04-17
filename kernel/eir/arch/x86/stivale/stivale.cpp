#include <stdint.h>
#include <assert.h>
#include <eir/interface.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/debug.hpp>
#include <acpispec/tables.h>

namespace eir {

struct [[gnu::packed]] Stivale2Struct {
	char brand[64];
	char version[64];

	uint64_t tags;
};

namespace TagIds {
	static constexpr uint64_t cmdline = 0xe5e76a1b4597a781;
	static constexpr uint64_t memoryMap = 0x2187f79e8612de07;
	static constexpr uint64_t framebuffer = 0x506461d2950408fa;
	static constexpr uint64_t modules = 0x4b6fe466aade04ce;
	static constexpr uint64_t rsdp = 0x9e1786930a375e78;
	static constexpr uint64_t dtb = 0xabb29bd49a2833fa;
} // namespace TagIds

struct [[gnu::packed]] Stivale2Tag {
	uint64_t identifier;
	uint64_t next;
};

enum class MmapEntryType : uint32_t {
	usable = 1,
	reserved,
	acpiReclaimable,
	acpiNvs,
	badMemory,
	bootloaderReclaimable = 0x1000,
	kernelAndModules
};

static constexpr const char *mmapEntryString(MmapEntryType t) {
	switch (t) {
		case MmapEntryType::usable: return "Usable";
		case MmapEntryType::reserved: return "Reserved";
		case MmapEntryType::acpiReclaimable: return "ACPI reclaimable";
		case MmapEntryType::acpiNvs: return "ACPI NVS";
		case MmapEntryType::badMemory: return "Bad memory";
		case MmapEntryType::bootloaderReclaimable: return "Bootloader reclaimable";
		case MmapEntryType::kernelAndModules: return "Kernel and modules";
		default: return "Invalid type";
	}
}

struct [[gnu::packed]] MmapEntry {
	uint64_t base;
	uint64_t length;
	MmapEntryType type;
	uint32_t rsvd;
};

struct [[gnu::packed]] Stivale2TagCmdline {
	uint64_t identifier;
	uint64_t next;
	uint64_t cmdline;
};

struct [[gnu::packed]] Stivale2TagMemoryMap {
	uint64_t identifier;
	uint64_t next;
	uint64_t nEntries;
	MmapEntry entries[];
};

struct [[gnu::packed]] Stivale2TagFramebuffer {
	uint64_t identifier;
	uint64_t next;
	uint64_t addr;
	uint16_t width;
	uint16_t height;
	uint16_t pitch;
	uint16_t bpp;
	uint8_t order;
	uint8_t red_mask_size;
	uint8_t red_mask_shift;
	uint8_t green_mask_size;
	uint8_t green_mask_shift;
	uint8_t blue_mask_size;
	uint8_t blue_mask_shift;
};

struct [[gnu::packed]] Stivale2Module {
	uint64_t begin;
	uint64_t end;
	char string[128];
};

struct [[gnu::packed]] Stivale2TagModules {
	uint64_t identifier;
	uint64_t next;
	uint64_t nEntries;
	Stivale2Module entries[];
};

struct [[gnu::packed]] Stivale2TagRsdp {
	uint64_t identifier;
	uint64_t next;
	uint64_t addr;
};

struct [[gnu::packed]] Stivale2TagDtb {
	uint64_t identifier;
	uint64_t next;
	uint64_t addr;
	uint64_t size;
};

extern "C" void eirEnterKernel(uintptr_t, uint64_t, uint64_t);

extern "C" void eirStivaleMain(Stivale2Struct *data) {
	const char *cmdline = "";
	frg::span<MmapEntry> mmap{nullptr, 0};
	frg::span<Stivale2Module> modules{nullptr, 0};
	uint64_t fbAddr = 0, fbWidth = 0, fbHeight = 0, fbPitch = 0;
	uint64_t rsdp = 0, dtbAddr = 0, dtbSize = 0;

	uintptr_t ptr = data->tags;
	while (ptr) {
		Stivale2Tag t;
		memcpy(&t, reinterpret_cast<void *>(ptr), sizeof(Stivale2Tag));

		switch (t.identifier) {
			case TagIds::framebuffer: {
				Stivale2TagFramebuffer t;
				memcpy(&t, reinterpret_cast<void *>(ptr), sizeof(Stivale2TagFramebuffer));

				if (t.bpp != 32) {
					eir::infoLogger()
						<< "eir: Framebuffer does not use 32 bpp!"
						<< frg::endlog;
					break;
				}

				fbAddr = t.addr;
				fbWidth = t.width;
				fbHeight = t.height;
				fbPitch = t.pitch;

				setFbInfo(reinterpret_cast<void *>(t.addr),
					t.width, t.height, t.pitch);
				break;
			}

			case TagIds::cmdline: {
				Stivale2TagCmdline t;
				memcpy(&t, reinterpret_cast<void *>(ptr), sizeof(Stivale2TagCmdline));
				cmdline = reinterpret_cast<const char *>(t.cmdline);
				break;
			}

			case TagIds::memoryMap: {
				Stivale2TagMemoryMap t;
				memcpy(&t, reinterpret_cast<void *>(ptr), sizeof(Stivale2TagMemoryMap));
				mmap = {reinterpret_cast<MmapEntry *>(ptr + sizeof(Stivale2TagMemoryMap)), t.nEntries};
				break;
			}

			case TagIds::modules: {
				Stivale2TagModules t;
				memcpy(&t, reinterpret_cast<void *>(ptr), sizeof(Stivale2TagModules));
				modules = {reinterpret_cast<Stivale2Module *>(ptr + sizeof(Stivale2TagModules)), t.nEntries};
				break;
			}

			case TagIds::rsdp: {
				Stivale2TagRsdp t;
				memcpy(&t, reinterpret_cast<void *>(ptr), sizeof(Stivale2TagRsdp));
				rsdp = t.addr;
				break;
			}

			case TagIds::dtb: {
				Stivale2TagDtb t;
				memcpy(&t, reinterpret_cast<void *>(ptr), sizeof(Stivale2TagDtb));
				dtbAddr = t.addr;
				dtbSize = t.size;
				break;
			}
		}

		ptr = t.next;
	}

	eir::infoLogger() << "Booted by: \"" << data->brand << "\", version: \""
		<< data->version << "\"" << frg::endlog;

	initProcessorEarly();

	eir::infoLogger() << "Memory map:" << frg::endlog;
	for (size_t i = 0; i < mmap.size(); i++) {
		auto ent = mmap.data()[i];

		eir::infoLogger() << "    Type: " << mmapEntryString(ent.type)
				<< ", Base: 0x" << frg::hex_fmt{ent.base}
				<< ", length: 0x" << frg::hex_fmt{ent.length} << frg::endlog;

		if (ent.type == MmapEntryType::usable)
			createInitialRegion(ent.base, ent.length);
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

	assert(modules.size() >= 2);
	auto kernelModule = modules.data()[0];

	uint64_t kernelEntry = 0;
	initProcessorPaging(reinterpret_cast<void *>(kernelModule.begin), kernelEntry);

	auto *infoPtr = generateInfo(cmdline);

	auto eirModules = bootAlloc<EirModule>(modules.size() - 1);
	for(size_t i = 0; i < modules.size() - 1; i++) {
		auto ent = modules.data()[i + 1];

		eirModules[i].physicalBase = ent.begin;
		eirModules[i].length = ent.end - ent.begin;

		size_t nameLength = strlen(ent.string);
		char *namePtr = bootAlloc<char>(nameLength);
		memcpy(namePtr, ent.string, nameLength);

		eirModules[i].namePtr = mapBootstrapData(namePtr);
		eirModules[i].nameLength = nameLength;
	}

	infoPtr->numModules = modules.size() - 1;
	infoPtr->moduleInfo = mapBootstrapData(eirModules);

	auto &framebuf = infoPtr->frameBuffer;
	framebuf.fbAddress = fbAddr;
	framebuf.fbPitch = fbPitch;
	framebuf.fbWidth = fbWidth;
	framebuf.fbHeight = fbHeight;
	framebuf.fbBpp = 32;
	framebuf.fbType = 0;

	if (rsdp) {
		acpi_xsdp_t *xsdpPtr = reinterpret_cast<acpi_xsdp_t *>(rsdp);

		if (xsdpPtr->revision == 0) {
			infoPtr->acpiRevision = 1;
			infoPtr->acpiRsdt = xsdpPtr->rsdt;
		} else {
			infoPtr->acpiRevision = 2;
			infoPtr->acpiRsdt = xsdpPtr->xsdt;
		}
	}

	infoPtr->dtbPtr = dtbAddr;
	infoPtr->dtbSize = dtbSize;

	// Map the framebuffer.
	assert(fbAddr & ~(pageSize - 1));
	for(address_t pg = 0; pg < fbPitch * fbHeight; pg += 0x1000)
		mapSingle4kPage(0xFFFF'FE00'4000'0000 + pg, fbAddr + pg,
				PageFlags::write, CachingMode::writeCombine);
	mapKasanShadow(0xFFFF'FE00'4000'0000, fbPitch * fbHeight);
	unpoisonKasanShadow(0xFFFF'FE00'4000'0000, fbPitch * fbHeight);
	framebuf.fbEarlyWindow = 0xFFFF'FE00'4000'0000;

	eir::infoLogger() << "Leaving Eir and entering the real kernel" << frg::endlog;
	eirEnterKernel(eirPml4Pointer, kernelEntry,
			0xFFFF'FE80'0001'0000);  
}

} // namespace eir
