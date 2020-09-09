#include <stdint.h>
#include <assert.h>
#include <eir/interface.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/debug.hpp>

namespace eir {

struct stivaleModule {
	uint64_t begin;
	uint64_t end;
	char     string[128];
	uint64_t next;
} __attribute__((packed));

struct stivaleStruct {
	uint64_t cmdline;
	uint64_t memoryMapAddr;
	uint64_t memoryMapEntries;
	uint64_t framebufferAddr;
	uint16_t framebufferPitch;
	uint16_t framebufferWidth;
	uint16_t framebufferHeight;
	uint16_t framebufferBpp;
	uint64_t rsdp;
	uint64_t moduleCount;
	uint64_t modules;
} __attribute__((packed));

struct e820Entry {
	uint64_t base;
	uint64_t length;
	uint32_t type;
	uint32_t unused;
} __attribute__((packed));

extern "C" void eirEnterKernel(uintptr_t, uint64_t, uint64_t);

extern "C" void eirStivaleMain(stivaleStruct* data){
	if(data->framebufferAddr + data->framebufferWidth * data->framebufferPitch >= UINT32_MAX) {
		eir::infoLogger() << "eir: Framebuffer outside of addressable memory!" << frg::endlog;
	}else if(data->framebufferBpp != 32){
		eir::infoLogger() << "eir: Framebuffer does not use 32 bpp!" << frg::endlog;
	}else{
		setFbInfo(reinterpret_cast<void *>(data->framebufferAddr),
				data->framebufferWidth, data->framebufferHeight, data->framebufferPitch);
	}

	initProcessorEarly();

	bootMemoryLimit = (uintptr_t)&eirImageCeiling;
	auto* module = (stivaleModule*)data->modules;
	for(size_t i = 0; i < data->moduleCount; i++){
		uintptr_t ceil = module->end;
		if(ceil > bootMemoryLimit)
			bootMemoryLimit = ceil;
		module = (stivaleModule*)module->next;
	}

	eir::infoLogger() << "Boot memory ceiling: " << frg::hex_fmt{bootMemoryLimit} << frg::endlog;

	bootMemoryLimit = (bootMemoryLimit + address_t(pageSize - 1))
			& ~address_t(pageSize - 1);

	eir::infoLogger() << "Memory map:" << frg::endlog;
	for(size_t i = 0; i < data->memoryMapEntries; i++) {
		auto map = &((e820Entry*)data->memoryMapAddr)[i];
		
		eir::infoLogger() << "    Type " << map->type << " mapping."
				<< " Base: 0x" << frg::hex_fmt{map->base}
				<< ", length: 0x" << frg::hex_fmt{map->length} << frg::endlog;
	}

	for(size_t i = 0; i < data->memoryMapEntries; i++) {
		auto map = &((e820Entry*)data->memoryMapAddr)[i];
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

	assert(data->moduleCount >= 2);
	stivaleModule& kernel_module = ((stivaleModule*)data->modules)[0];

	uint64_t kernel_entry = 0;
	initProcessorPaging((void *)kernel_module.begin, kernel_entry);

	auto *info_ptr = generateInfo((const char*)data->cmdline);

	auto modules = bootAlloc<EirModule>(data->moduleCount - 1);
	module = (stivaleModule*)data->modules;
	module = (stivaleModule*)module->next; // Skip kernel
	for(size_t i = 0; i < data->moduleCount - 1; i++) {
		modules[i].physicalBase = (EirPtr)module->begin;
		modules[i].length = (EirPtr)module->end
				- (EirPtr)module->begin;

		size_t name_length = strlen(module->string);
		char *name_ptr = bootAlloc<char>(name_length);
		memcpy(name_ptr, module->string, name_length);
		modules[i].namePtr = mapBootstrapData(name_ptr);
		modules[i].nameLength = name_length;
		module = (stivaleModule*)module->next;
	}

	info_ptr->numModules = data->moduleCount - 1;
	info_ptr->moduleInfo = mapBootstrapData(modules);
	
	auto& framebuf = info_ptr->frameBuffer;
	framebuf.fbAddress = data->framebufferAddr;
	framebuf.fbPitch = data->framebufferPitch;
	framebuf.fbWidth = data->framebufferWidth;
	framebuf.fbHeight = data->framebufferHeight;
	framebuf.fbBpp = data->framebufferBpp;
	framebuf.fbType = 0;
	
	// Map the framebuffer.
	assert(data->framebufferAddr & ~(pageSize - 1));
	for(address_t pg = 0; pg < data->framebufferPitch * data->framebufferHeight; pg += 0x1000)
		mapSingle4kPage(0xFFFF'FE00'4000'0000 + pg, data->framebufferAddr + pg,
				PageFlags::write, CachingMode::writeCombine);
	mapKasanShadow(0xFFFF'FE00'4000'0000, data->framebufferPitch * data->framebufferHeight);
	unpoisonKasanShadow(0xFFFF'FE00'4000'0000, data->framebufferPitch * data->framebufferHeight);
	framebuf.fbEarlyWindow = 0xFFFF'FE00'4000'0000;

	eir::infoLogger() << "Leaving Eir and entering the real kernel" << frg::endlog;
	eirEnterKernel(eirPml4Pointer, kernel_entry,
			0xFFFF'FE80'0001'0000);  
}

} // namespace eir
