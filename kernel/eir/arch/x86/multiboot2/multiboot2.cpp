#include <stdint.h>
#include <frigg/c-support.h>
#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>
#include <frigg/debug.hpp>
#include <frigg/support.hpp>
#include <frigg/string.hpp>
#include <frigg/libc.hpp>
#include <eir/interface.hpp>
#include "../main.hpp"

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

extern "C" void eirMultiboot2Main(uint32_t info, uint32_t magic){
	if(magic != 0x36d76289)
		frigg::panicLogger() << "eir: Invalid multiboot2 signature, halting..." << frigg::endLog;

	bootMemoryLimit = (uintptr_t)&eirImageCeiling; // Check if any modules are higher during iteration

	Mb2Info* mb_info = reinterpret_cast<Mb2Info*>(info);
	size_t add_size = 0;

	Mb2TagFramebuffer* framebuffer = nullptr;

	uintptr_t mmap_start = 0;
	uintptr_t mmap_end = 0;

	size_t n_modules = 0;

	uintptr_t kernel_module_start = 0;
	
	frigg::StringView cmdline{};

	for(size_t i = 8 /* Skip size and reserved fields*/; i < mb_info->size; i += add_size){
		Mb2Tag* tag = (Mb2Tag*)((uint8_t*)info + i);

		if(tag->type == kMb2TagEnd)
			break;

		add_size = tag->size;
		if((add_size % 8)!= 0) 
			add_size += (8 - add_size % 8); // Align 8byte

		switch (tag->type)
		{
			case kMb2TagFramebuffer: {
				auto *framebuffer_tag = reinterpret_cast<Mb2TagFramebuffer*>(tag);
				if(framebuffer_tag->address + framebuffer_tag->width * framebuffer_tag->pitch >= UINTPTR_MAX) {
					frigg::panicLogger() << "eir: Framebuffer outside of addressable memory!"
						<< frigg::endLog;
				}else if(framebuffer_tag->bpp != 32) {
					frigg::panicLogger() << "eir: Framebuffer does not use 32 bpp!"
						<< frigg::endLog;
				}else{
					displayFb = reinterpret_cast<void *>(framebuffer->address);
					displayWidth = framebuffer_tag->width;
					displayHeight = framebuffer_tag->height;
					displayPitch = framebuffer_tag->pitch;

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
		}
	}

	bootMemoryLimit = (bootMemoryLimit + address_t(kPageSize - 1))
			& ~address_t(kPageSize - 1);

	initProcessorEarly();

	assert(mmap_start);
	assert(mmap_end > mmap_start);

	assert(n_modules >= 2);

	assert(cmdline.data()); // Make sure it at least exists
	assert(framebuffer);

	frigg::infoLogger() << "Command line: " << cmdline << frigg::endLog;

	frigg::infoLogger() << "Memory map:" << frigg::endLog;
	for(Mb2MmapEntry* map = (Mb2MmapEntry*)mmap_start; map < (Mb2MmapEntry*)mmap_end; map++) {		
		frigg::infoLogger() << "    Type " << map->type << " mapping."
				<< " Base: 0x" << frigg::logHex(map->base)
				<< ", length: 0x" << frigg::logHex(map->length) << frigg::endLog;
	}

	for(Mb2MmapEntry* map = (Mb2MmapEntry*)mmap_start; map < (Mb2MmapEntry*)mmap_end; map++) {
		if(map->type == 1)
			createInitialRegion(map->base, map->length);
	}
	setupRegionStructs();

	frigg::infoLogger() << "Kernel memory regions:" << frigg::endLog;
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType == RegionType::null)
			continue;
		frigg::infoLogger() << "    Memory region [" << i << "]."
				<< " Base: 0x" << frigg::logHex(regions[i].address)
				<< ", length: 0x" << frigg::logHex(regions[i].size) << frigg::endLog;
		if(regions[i].regionType == RegionType::allocatable)
			frigg::infoLogger() << "        Buddy tree at 0x" << frigg::logHex(regions[i].buddyTree)
					<< ", overhead: 0x" << frigg::logHex(regions[i].buddyOverhead)
					<< frigg::endLog;
	}

	uint64_t kernel_entry = 0;
	initProcessorPaging((void *)kernel_module_start, kernel_entry);

	auto *info_ptr = generateInfo(cmdline.data());

	auto modules = bootAllocN<EirModule>(n_modules - 1);
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
				char *name_ptr = bootAllocN<char>(name_length);
				memcpy(name_ptr, module->string, name_length);
				modules[j - 2].namePtr = mapBootstrapData(name_ptr);
				modules[j - 2].nameLength = name_length;
				break;
			}
		}
	}

	info_ptr->numModules = n_modules - 1;
	info_ptr->moduleInfo = mapBootstrapData(modules);
	
	auto framebuf = &info_ptr->frameBuffer;
	framebuf->fbAddress = framebuffer->address;
	framebuf->fbPitch = framebuffer->pitch;
	framebuf->fbWidth = framebuffer->width;
	framebuf->fbHeight = framebuffer->height;
	framebuf->fbBpp = framebuffer->bpp;
	framebuf->fbType = framebuffer->type;

	// Map the framebuffer.
	assert(framebuffer->address & ~(kPageSize - 1));
	for(address_t pg = 0; pg < framebuffer->pitch * framebuffer->height; pg += 0x1000)
		mapSingle4kPage(0xFFFF'FE00'4000'0000 + pg, framebuffer->address + pg,
				kAccessWrite, CachingMode::writeCombine);
	mapKasanShadow(0xFFFF'FE00'4000'0000, framebuffer->pitch * framebuffer->height);
	unpoisonKasanShadow(0xFFFF'FE00'4000'0000, framebuffer->pitch * framebuffer->height);
	framebuf->fbEarlyWindow = 0xFFFF'FE00'4000'0000;
	
	frigg::infoLogger() << "Leaving Eir and entering the real kernel" << frigg::endLog;
	eirEnterKernel(eirPml4Pointer, kernel_entry,
			0xFFFF'FE80'0001'0000);  
}
