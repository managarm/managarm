#include <limine.h>
#include <stdint.h>
#include <assert.h>
#include <eir/interface.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/arch/riscv/paging.hpp>
#include <eir-internal/arch/types.hpp>
#include "limine.h"

namespace eir {

static volatile struct limine_framebuffer_request framebuffer_request = {
	.id = LIMINE_FRAMEBUFFER_REQUEST,
	.revision = 0
};

static volatile struct limine_dtb_request dtb_request = {
	.id = LIMINE_DTB_REQUEST,
	.revision = 0
};

static volatile struct limine_rsdp_request rsdp_request = {
	.id = LIMINE_RSDP_REQUEST,
	.revision = 0
};

static volatile struct limine_memmap_request memmap_request = {
	.id = LIMINE_MEMMAP_REQUEST,
	.revision = 0
};

static volatile struct limine_paging_mode_request paging_request = {
	.id = LIMINE_PAGING_MODE_REQUEST,
	.revision = 0,
	.mode = LIMINE_PAGING_MODE_RISCV_SV39
};

static volatile struct limine_kernel_address_request kernel_address_request = {
	.id = LIMINE_KERNEL_ADDRESS_REQUEST,
	.revision = 0
};

static volatile struct limine_module_request module_request = {
	.id = LIMINE_MODULE_REQUEST,
	.revision = 0
};

typedef unsigned long SbiWord;

void sbiCall1(int ext, int func, SbiWord arg0) {
	register SbiWord rExt asm("a7") = ext;
	register SbiWord rFunc asm("a6") = func;
	register SbiWord rArg0 asm("a0") = arg0;
	register SbiWord rArg1 asm("a1");
	asm volatile("ecall" : "+r"(rArg0), "=r"(rArg1) : "r"(rExt), "r"(rFunc));
	if(rArg0)
		__builtin_trap();
}


void debugPrintChar(char c) {
	sbiCall1(1, 0, c);
}


// We need to know where eir is, so that we can map it.
// Therefore, we place the processor paging init code here.
// TODO: do we even need to pretend this is initProcessorPaging?
//       we take almost everything from the limine variables anyway...
void initProcessorPaging(void *kernel_start, uint64_t &kernel_entry) {
	// We are technically already paged, but we need to create another new page table.
	eir::infoLogger() << "eir: initialising paging with eir in higher half" << frg::endlog;

	// The kernel file.
	struct limine_file* kernel_file = module_request.response->modules[0];

	// Basically:
	//  - we create a page table but do not switch to it yet
	//  - we identity map lower (usable) memory
	//  - we map eir to its higher half address
	//  - we copy and map the kernel image
	//  - we copy and map the kernel's modules
	//  - we switch to the new page table
	//
	// This means we will have done quite a bit outside of the normal eir functions,
	// but we need to do this in order to cope with eir being loaded high,
	// and not knowing with 100% certanty where limine's structures are loaded in
	// lower memory.
	// If we knew the offsets which we needed to map for the limine structures,
	// we could skip most of these.

	// Allocate and clear the top page table.
	pt2 = (sv39_page_table_entry*)allocPage();
	eir::infoLogger() << "eir: allocating top level page table at " << frg::hex_fmt{pt2} << frg::endlog;
	memset(pt2, 0, 4096);

	// Identity-map all memory which is marked as usable.
	for(size_t i = 0; i < memmap_request.response->entry_count; i++) {
		struct limine_memmap_entry* entry = memmap_request.response->entries[i];
		switch(entry->type) {
		case LIMINE_MEMMAP_USABLE:
			eir::infoLogger() << "eir: mapping limine entry " << i << frg::endlog;
			for(address_t addr = entry->base; addr < (entry->base + entry->length); addr += pageSize) {
				mapSingle4kPage(addr, addr, PageFlags::write, CachingMode::null);
			}
			break;
		case LIMINE_MEMMAP_RESERVED:
		case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
		case LIMINE_MEMMAP_ACPI_NVS:
		case LIMINE_MEMMAP_BAD_MEMORY:
		case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
		case LIMINE_MEMMAP_KERNEL_AND_MODULES:
		case LIMINE_MEMMAP_FRAMEBUFFER:
			break;	
		default:
			eir::panicLogger() << "eir: \tInvalid memory map type: " << entry->type << frg::endlog;
		}
	}

	eir::infoLogger() << "eir: processor paging inited" << frg::endlog;
}

// Limine entry point.
extern "C" void efiStart() {
	// This can't possibly go wrong...
	//InitialRegion reservedRegions[32];
	//size_t nReservedRegions = 0;

	eir::infoLogger() << "eir: efiStart()" << frg::endlog;
	
	// Dump memory regions
	eir::infoLogger() << "eir: Memory Map from Limine:" << frg::endlog;
	for(size_t i = 0; i < memmap_request.response->entry_count; i++) {
		struct limine_memmap_entry* entry = memmap_request.response->entries[i];
		eir::infoLogger() << "eir: Entry " << i << frg::endlog;
		eir::infoLogger() << "eir: \tbase: 0x" << frg::hex_fmt{entry->base} << frg::endlog;
		eir::infoLogger() << "eir: \tsize: 0x" << frg::hex_fmt{entry->length} << frg::endlog;
	
		switch(entry->type) {
		case LIMINE_MEMMAP_USABLE:
			createInitialRegion(entry->base, entry->length);
			eir::infoLogger() << "eir: \ttype: usable" << frg::endlog;
			break;
		case LIMINE_MEMMAP_RESERVED:
			eir::infoLogger() << "eir: \ttype: reserved" << frg::endlog;	
			break;
		case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
			eir::infoLogger() << "eir: \ttype: acpi reclaimable" << frg::endlog;	
			break;
		case LIMINE_MEMMAP_ACPI_NVS:
			eir::infoLogger() << "eir: \ttype: acpi nvs" << frg::endlog;	
			break;
		case LIMINE_MEMMAP_BAD_MEMORY:
			eir::infoLogger() << "eir: \ttype: bad memory" << frg::endlog;	
			break;
		case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
			eir::infoLogger() << "eir: \ttype: bootloader reclaimable" << frg::endlog;	
			break;
		case LIMINE_MEMMAP_KERNEL_AND_MODULES:
			eir::infoLogger() << "eir: \ttype: kernel and modules" << frg::endlog;	
			break;
		case LIMINE_MEMMAP_FRAMEBUFFER:
			eir::infoLogger() << "eir: \ttype: framebuffer" << frg::endlog;	
			break;	
		default:
			eir::panicLogger() << "eir: \tInvalid memory map type: " << entry->type << frg::endlog;
		}
	}

	initProcessorEarly();

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

	// TODO: have a thor to load i guess
	// for testing i just loaded eir two more times lol
	assert(module_request.response->module_count >= 2);
	void* kernel_module_begin = module_request.response->modules[0]->address;
	eir::infoLogger() << "eir: kernel module base address in memory is 0x" << frg::hex_fmt{kernel_module_begin} << frg::endlog;

	uint64_t kernel_entry;
	initProcessorPaging(kernel_module_begin, kernel_entry);

	while(true);
}

}