
#include "../../../frigg/include/arch_x86/types64.hpp"
#include "../runtime.hpp"
#include "../debug.hpp"
#include "physical-alloc.hpp"
#include "paging.hpp"

namespace thor {
namespace memory {

LazyInitializer<PageSpace> kernelSpace;

void *physicalToVirtual(uintptr_t address) {
	return (void *)address;
}

// --------------------------------------------------------
// PageSpace
// --------------------------------------------------------

PageSpace::PageSpace(uintptr_t pml4_address)
			: p_pml4Address(pml4_address) { }

void PageSpace::mapSingle4k(void *pointer, uintptr_t physical) {
	if((uintptr_t)pointer % 0x1000 != 0) {
		debug::criticalLogger->log("pk_page_map(): Illegal virtual address alignment");
		debug::panic();
	}
	if(physical % 0x1000 != 0) {
		debug::criticalLogger->log("pk_page_map(): Illegal physical address alignment");
		debug::panic();
	}

	int pml4_index = (int)(((uintptr_t)pointer >> 39) & 0x1FF);
	int pdpt_index = (int)(((uintptr_t)pointer >> 30) & 0x1FF);
	int pd_index = (int)(((uintptr_t)pointer >> 21) & 0x1FF);
	int pt_index = (int)(((uintptr_t)pointer >> 12) & 0x1FF);
	
	// find the pml4_entry. the pml4 exists already
	volatile uint64_t *pml4 = (uint64_t *)physicalToVirtual(p_pml4Address);
	uint64_t pml4_entry = ((uint64_t*)pml4)[pml4_index];
	
	// find the pdpt entry; create pdpt if necessary
	volatile uint64_t *pdpt = (uint64_t *)physicalToVirtual(pml4_entry & 0xFFFFF000);
	if((pml4_entry & kPagePresent) == 0) {
		uintptr_t page = tableAllocator->allocate();
		debug::criticalLogger->log("allocate pdpt");
		pdpt = (uint64_t *)physicalToVirtual(page);
		for(int i = 0; i < 512; i++)
			((uint64_t*)pdpt)[i] = 0;
		pml4[pml4_index] = page | kPagePresent | kPageWrite | kPageUser;
	}
	uint64_t pdpt_entry = ((uint64_t*)pdpt)[pdpt_index];
	
	// find the pd entry; create pd if necessary
	volatile uint64_t *pd = (uint64_t *)physicalToVirtual(pdpt_entry & 0xFFFFF000);
	if((pdpt_entry & kPagePresent) == 0) {
		uintptr_t page = tableAllocator->allocate();
		debug::criticalLogger->log("allocate pd");
		pd = (uint64_t *)physicalToVirtual(page);
		for(int i = 0; i < 512; i++)
			((uint64_t*)pd)[i] = 0;
		pdpt[pdpt_index] = page | kPagePresent | kPageWrite | kPageUser;
	}
	uint64_t pd_entry = ((uint64_t*)pd)[pd_index];
	
	// find the pt entry; create pt if necessary
	volatile uint64_t *pt = (uint64_t *)physicalToVirtual(pd_entry & 0xFFFFF000);
	if((pd_entry & kPagePresent) == 0) {
		uintptr_t page = tableAllocator->allocate();
		debug::criticalLogger->log("allocate pt");
		pt = (uint64_t *)physicalToVirtual(page);
		for(int i = 0; i < 512; i++)
			((uint64_t*)pt)[i] = 0;
		pd[pd_index] = page | kPagePresent | kPageWrite | kPageUser;
	}
	
	// setup the new pt entry
	if((pt[pt_index] & kPagePresent) != 0) {
		debug::criticalLogger->log("pk_page_map(): Page already mapped!");
		debug::panic();
	}
	pt[pt_index] = physical | kPagePresent | kPageWrite | kPageUser;
}

}} // namespace thor::memory

