#include <stdint.h>
#include <assert.h>
#include <eir/interface.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/debug.hpp>
#include <acpispec/tables.h>

namespace eir {

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
		struct
		{
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

struct Mb2TagRSDPv1 {
	uint32_t type;
	uint32_t length;

	struct acpi_rsdp_t rsdp;
};

struct Mb2TagRSDPv2 {
	uint32_t type;
	uint32_t length;

	struct acpi_xsdp_t xsdp;
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

extern "C" void eirEnterKernel(uintptr_t, uint64_t, uint64_t);

extern "C" void eirMultiboot2Main(uint32_t info, uint32_t magic){
	if(magic != 0x36d76289)
		eir::panicLogger() << "eir: Invalid multiboot2 signature, halting..." << frg::endlog;

	bootMemoryLimit = (uintptr_t)&eirImageCeiling; // Check if any modules are higher during iteration

	Mb2Info* mb_info = reinterpret_cast<Mb2Info*>(info);
	size_t add_size = 0;

	Mb2TagFramebuffer* framebuffer = nullptr;

	uintptr_t mmap_start = 0;
	uintptr_t mmap_end = 0;

	size_t n_modules = 0;

	uintptr_t kernel_module_start = 0;
	
	frg::string_view cmdline{};

	uint64_t rsdt = 0;
	uint64_t acpiRevision = 0;

	for(size_t i = 8 /* Skip size and reserved fields*/; i < mb_info->size; i += add_size){
		Mb2Tag* tag = (Mb2Tag*)((uint8_t*)info + i);

		if(tag->type == kMb2TagEnd)
			break;

		add_size = tag->size;
		if((add_size % 8)!= 0) 
			add_size += (8 - add_size % 8); // Align 8byte

		switch (tag->type) {
			case kMb2TagFramebuffer: {
				auto *framebuffer_tag = reinterpret_cast<Mb2TagFramebuffer*>(tag);
				if(framebuffer_tag->address + framebuffer_tag->width * framebuffer_tag->pitch >= UINTPTR_MAX) {
					eir::panicLogger() << "eir: Framebuffer outside of addressable memory!"
						<< frg::endlog;
				}else if(framebuffer_tag->bpp != 32) {
					eir::panicLogger() << "eir: Framebuffer does not use 32 bpp!"
						<< frg::endlog;
				}else{
					setFbInfo(reinterpret_cast<void *>(framebuffer->address),
							framebuffer_tag->width, framebuffer_tag->height, framebuffer_tag->pitch);

					framebuffer = framebuffer_tag;
				}
				break;
			}

			case kMb2TagModule: {
				auto *module = reinterpret_cast<Mb2TagModule*>(tag);

				n_modules++;
				if(n_modules == 1){ // First module so kernel
					kernel_module_start = (uintptr_t)module->start;
				}

				uintptr_t ceil = (uintptr_t)module->end;
				if(ceil > bootMemoryLimit)
					bootMemoryLimit = ceil;
				break;
			}

			case kMb2TagMmap: {
				auto *mmap = reinterpret_cast<Mb2TagMmap*>(tag);

				mmap_start = (uintptr_t)mmap->entries;
				mmap_end = (uintptr_t)mmap + mmap->length;

				break;
			}

			case kMb2TagCmdline: {
				auto *cmdline_tag = reinterpret_cast<Mb2TagCmdline*>(tag);

				cmdline = {cmdline_tag->string};

				break;
			}

			case kMb2TagAcpiOld: {
				auto *rsdp_tag = reinterpret_cast<Mb2TagRSDPv1*>(tag);

				if(acpiRevision)
					eir::infoLogger() << "eir: Parsing old acpi tag but acpiRevision is alreay set?" << frg::endlog;
				rsdt = rsdp_tag->rsdp.rsdt;
				acpiRevision = 1;

				break;
			}

			case kMb2TagAcpiNew: {
				auto *rsdp_tag = reinterpret_cast<Mb2TagRSDPv2*>(tag);

				if(acpiRevision)
					eir::infoLogger() << "eir: Parsing new acpi tag but acpiRevision is alreay set?" << frg::endlog;
				rsdt = rsdp_tag->xsdp.xsdt;
				acpiRevision = 2;

				break;
			}
		}
	}

	bootMemoryLimit = (bootMemoryLimit + address_t(pageSize - 1))
			& ~address_t(pageSize - 1);

	initProcessorEarly();

	assert(mmap_start);
	assert(mmap_end > mmap_start);

	assert(n_modules >= 2);

	assert(cmdline.data()); // Make sure it at least exists
	assert(framebuffer);

	eir::infoLogger() << "Command line: " << cmdline << frg::endlog;

	eir::infoLogger() << "Memory map:" << frg::endlog;
	for(Mb2MmapEntry* map = (Mb2MmapEntry*)mmap_start; map < (Mb2MmapEntry*)mmap_end; map++) {		
		eir::infoLogger() << "    Type " << map->type << " mapping."
				<< " Base: 0x" << frg::hex_fmt{map->base}
				<< ", length: 0x" << frg::hex_fmt{map->length} << frg::endlog;
	}

	for(Mb2MmapEntry* map = (Mb2MmapEntry*)mmap_start; map < (Mb2MmapEntry*)mmap_end; map++) {
		if(map->type == 1)
			createInitialRegion(map->base, map->length);
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

	uint64_t kernel_entry = 0;
	initProcessorPaging((void *)kernel_module_start, kernel_entry);

	auto *info_ptr = generateInfo(cmdline.data());

	auto modules = bootAlloc<EirModule>(n_modules - 1);
	add_size = 0;

	size_t j = 0; // Module index
	for(size_t i = 8 /* Skip size and reserved fields*/; i < mb_info->size; i += add_size){
		Mb2Tag* tag = (Mb2Tag*)((uint8_t*)info + i);

		if(tag->type == kMb2TagEnd)
			break;

		add_size = tag->size;
		if((add_size % 8)!= 0) add_size += (8 - add_size % 8); // Align 8byte

		switch (tag->type)
		{
			case kMb2TagModule: {
				auto *module = reinterpret_cast<Mb2TagModule*>(tag);
				j++;
				if(j == 1)
					break; // Skip first module, it is the kernel

				
				modules[j - 2].physicalBase = (EirPtr)module->start;
				modules[j - 2].length = (EirPtr)module->end - (EirPtr)module->start;

				size_t name_length = strlen(module->string);
				char *name_ptr = bootAlloc<char>(name_length);
				memcpy(name_ptr, module->string, name_length);
				modules[j - 2].namePtr = mapBootstrapData(name_ptr);
				modules[j - 2].nameLength = name_length;
				break;
			}
		}
	}

	info_ptr->numModules = n_modules - 1;
	info_ptr->moduleInfo = mapBootstrapData(modules);
	info_ptr->acpiRevision = acpiRevision;
	info_ptr->acpiRsdt = rsdt;
	
	auto framebuf = &info_ptr->frameBuffer;
	framebuf->fbAddress = framebuffer->address;
	framebuf->fbPitch = framebuffer->pitch;
	framebuf->fbWidth = framebuffer->width;
	framebuf->fbHeight = framebuffer->height;
	framebuf->fbBpp = framebuffer->bpp;
	framebuf->fbType = framebuffer->type;

	// Map the framebuffer.
	assert(framebuffer->address & ~(pageSize - 1));
	for(address_t pg = 0; pg < framebuffer->pitch * framebuffer->height; pg += 0x1000)
		mapSingle4kPage(0xFFFF'FE00'4000'0000 + pg, framebuffer->address + pg,
				PageFlags::write, CachingMode::writeCombine);
	mapKasanShadow(0xFFFF'FE00'4000'0000, framebuffer->pitch * framebuffer->height);
	unpoisonKasanShadow(0xFFFF'FE00'4000'0000, framebuffer->pitch * framebuffer->height);
	framebuf->fbEarlyWindow = 0xFFFF'FE00'4000'0000;
	
	eir::infoLogger() << "Leaving Eir and entering the real kernel" << frg::endlog;
	eirEnterKernel(eirPml4Pointer, kernel_entry,
			0xFFFF'FE80'0001'0000);  
}

} // namespace eir
