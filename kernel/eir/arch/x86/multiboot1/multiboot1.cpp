#include <stdint.h>
#include <assert.h>
#include <eir/interface.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/debug.hpp>

#include <acpispec/tables.h>

namespace eir {

enum MbInfoFlags {
	kMbInfoPlainMemory = (1 << 0),
	kMbInfoBootDevice = (1 << 1),
	kMbInfoCommandLine = (1 << 2),
	kMbInfoModules = (1 << 3),
	kMbInfoSymbols = (1 << 5),
	kMbInfoMemoryMap = (1 << 6),
	kMbInfoFramebuffer = (1 << 12)
};

struct MbModule {
	void *startAddress;
	void *endAddress;
	char *string;
	uint32_t reserved;
};

struct MbInfo {
	uint32_t flags;
	uint32_t memLower;
	uint32_t memUpper;
	uint32_t bootDevice;
	char *commandLine;
	uint32_t numModules;
	MbModule *modulesPtr;
	uint32_t numSymbols;
	uint32_t symbolSize;
	void *symbolsPtr;
	uint32_t stringSection;
	uint32_t memoryMapLength;
	void *memoryMapPtr;
	uint32_t padding[9];
	uint64_t fbAddress;
	uint32_t fbPitch;
	uint32_t fbWidth;
	uint32_t fbHeight;
	uint8_t fbBpp;
	uint8_t fbType;
	uint8_t colorInfo[6];
};

struct MbMemoryMap {
	uint32_t size;
	uint64_t baseAddress;
	uint64_t length;
	uint32_t type;
};

static bool findAcpiRsdp(EirInfo *info) {
	auto doChecksum = [](void *ptr, size_t len) {
		uint8_t checksum = 0;
		for(size_t i = 0; i < len; i++)
			checksum += reinterpret_cast<uint8_t *>(ptr)[i];
		return checksum;
	};

	auto scanZone = [info, doChecksum](uintptr_t base, size_t len) {
		uint8_t *region = reinterpret_cast<uint8_t *>(base);
	
		for(size_t off = 0; off < len; off += 16) {
			acpi_rsdp_t *rsdp = reinterpret_cast<acpi_rsdp_t *>(region + off);

			if(memcmp(rsdp->signature, "RSD PTR ", 8))
				continue;

			if(doChecksum(static_cast<void *>(rsdp), sizeof(acpi_rsdp_t)) != 0)
				continue;

			if(!rsdp->revision) {
				info->acpiRevision = 1;
				info->acpiRsdt = rsdp->rsdt;
				return true;
			} else {
				acpi_xsdp_t *xsdp = reinterpret_cast<acpi_xsdp_t *>(rsdp);

				if(doChecksum(static_cast<void *>(xsdp), sizeof(acpi_xsdp_t)))
					continue;

				info->acpiRevision = 2;
				info->acpiRsdt = xsdp->xsdt;
				return true;
			}
		}

		return false;
	};

	// First, find the base address of the EDBA
	uint16_t bdaData = *reinterpret_cast<uint16_t *>(0x40E);
	uintptr_t ebdaBase = ((uintptr_t)bdaData) << 4;

	// Next, try the EDBA
	if(scanZone(ebdaBase, 0x400))
		return true;

	// Finally, try the BIOS memory
	if(scanZone(0xE0000, 0x20000))
		return true;
  
	return false;
}

extern "C" void eirEnterKernel(uintptr_t, uint64_t, uint64_t);

extern "C" void eirMultiboot1Main(uint32_t info, uint32_t magic){
	if(magic != 0x2BADB002)
		eir::panicLogger() << "eir: Invalid multiboot1 signature, halting..." << frg::endlog;

	MbInfo* mb_info = reinterpret_cast<MbInfo*>(info);

	if(mb_info->flags & kMbInfoFramebuffer) {
		if(mb_info->fbAddress + mb_info->fbWidth * mb_info->fbPitch >= UINTPTR_MAX) {
			eir::infoLogger() << "eir: Framebuffer outside of addressable memory!"
					<< frg::endlog;
		}else if(mb_info->fbBpp != 32) {
			eir::infoLogger() << "eir: Framebuffer does not use 32 bpp!"
					<< frg::endlog;
		}else{
			setFbInfo(reinterpret_cast<void *>(mb_info->fbAddress), mb_info->fbWidth,
					mb_info->fbHeight, mb_info->fbPitch);
		}
	}

	initProcessorEarly();

	InitialRegion reservedRegions[32];
	size_t nReservedRegions = 0;

	uintptr_t eirEnd = reinterpret_cast<uintptr_t>(&eirImageCeiling);
	reservedRegions[nReservedRegions++] = {0, eirEnd};

	if((mb_info->flags & kMbInfoModules) != 0) {
		for(unsigned int i = 0; i < mb_info->numModules; i++) {
			uintptr_t start = (uintptr_t)mb_info->modulesPtr[i].startAddress;
			uintptr_t end = (uintptr_t)mb_info->modulesPtr[i].endAddress;
			reservedRegions[nReservedRegions++] = {start, end - start};
		}
	}

	// Walk the memory map and retrieve all useable regions.
	assert(mb_info->flags & kMbInfoMemoryMap);
	eir::infoLogger() << "Memory map:" << frg::endlog;
	for(size_t offset = 0; offset < mb_info->memoryMapLength; ) {
		auto map = (MbMemoryMap *)((uintptr_t)mb_info->memoryMapPtr + offset);

		eir::infoLogger() << "    Type " << map->type << " mapping."
				<< " Base: 0x" << frg::hex_fmt{map->baseAddress}
				<< ", length: 0x" << frg::hex_fmt{map->length} << frg::endlog;

		offset += map->size + 4;
	}

	for(size_t offset = 0; offset < mb_info->memoryMapLength; ) {
		auto map = (MbMemoryMap *)((uintptr_t)mb_info->memoryMapPtr + offset);

		if(map->type == 1)
			createInitialRegions({map->baseAddress, map->length}, {reservedRegions, nReservedRegions});

		offset += map->size + 4;
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

	assert((mb_info->flags & kMbInfoModules) != 0);
	assert(mb_info->numModules >= 2);
	MbModule *kernel_module = &mb_info->modulesPtr[0];

	uint64_t kernel_entry = 0;
	initProcessorPaging((void *)kernel_module->startAddress, kernel_entry);

	assert(mb_info->flags & kMbInfoCommandLine);
	auto *info_ptr = generateInfo(mb_info->commandLine);

	auto modules = bootAlloc<EirModule>(mb_info->numModules - 1);
	for(size_t i = 0; i < mb_info->numModules - 1; i++) {
		MbModule &image_module = mb_info->modulesPtr[i + 1];
		modules[i].physicalBase = (EirPtr)image_module.startAddress;
		modules[i].length = (EirPtr)image_module.endAddress
				- (EirPtr)image_module.startAddress;

		size_t name_length = strlen(image_module.string);
		char *name_ptr = bootAlloc<char>(name_length);
		memcpy(name_ptr, image_module.string, name_length);
		modules[i].namePtr = mapBootstrapData(name_ptr);
		modules[i].nameLength = name_length;
	}

	info_ptr->numModules = mb_info->numModules - 1;
	info_ptr->moduleInfo = mapBootstrapData(modules);

	// Manually probe for ACPI tables in EBDA/BIOS memory
	if(!findAcpiRsdp(info_ptr))
		eir::panicLogger() << "eir: unable to find ACPI RSDP in low memory, halting..." << frg::endlog;

	if((mb_info->flags & kMbInfoFramebuffer)
			&& (mb_info->fbType == 1)) { // For now, only linear framebuffer is supported.
		auto framebuf = &info_ptr->frameBuffer;
		framebuf->fbAddress = mb_info->fbAddress;
		framebuf->fbPitch = mb_info->fbPitch;
		framebuf->fbWidth = mb_info->fbWidth;
		framebuf->fbHeight = mb_info->fbHeight;
		framebuf->fbBpp = mb_info->fbBpp;
		framebuf->fbType = mb_info->fbType;

		assert(mb_info->fbAddress & ~(pageSize - 1));
		for(address_t pg = 0; pg < mb_info->fbPitch * mb_info->fbHeight; pg += 0x1000)
			mapSingle4kPage(0xFFFF'FE00'4000'0000 + pg, mb_info->fbAddress + pg,
					PageFlags::write, CachingMode::writeCombine);
		mapKasanShadow(0xFFFF'FE00'4000'0000, mb_info->fbPitch * mb_info->fbHeight);
		unpoisonKasanShadow(0xFFFF'FE00'4000'0000, mb_info->fbPitch * mb_info->fbHeight);
		framebuf->fbEarlyWindow = 0xFFFF'FE00'4000'0000;
	}

	eir::infoLogger() << "Leaving Eir and entering the real kernel" << frg::endlog;

	eirEnterKernel(eirPml4Pointer, kernel_entry,
			0xFFFF'FE80'0001'0000);  
}

} // namespace eir
