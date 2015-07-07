
#include "../../../frigg/include/arch_x86/types64.hpp"
#include "../util/general.hpp"
#include "../runtime.hpp"
#include "../debug.hpp"
#include "physical-alloc.hpp"
#include "paging.hpp"

namespace thor {
namespace memory {

LazyInitializer<PageSpace> kernelSpace;

void *physicalToVirtual(uintptr_t address) {
	return (void *)(0xFFFF800100000000 + address);
}

// --------------------------------------------------------
// PageSpace
// --------------------------------------------------------

PageSpace::PageSpace(uintptr_t pml4_address)
			: p_pml4Address(pml4_address) { }

void PageSpace::switchTo() {
	asm volatile ( "mov %0, %%cr3" : : "r"( p_pml4Address ) );
}

PageSpace PageSpace::clone() {
	uint64_t new_pml4_page = tableAllocator->allocate();
	volatile uint64_t *this_pml4_pointer = (uint64_t *)physicalToVirtual(p_pml4Address);
	volatile uint64_t *new_pml4_pointer = (uint64_t *)physicalToVirtual(new_pml4_page);

	for(int i = 0; i < 256; i++)
		new_pml4_pointer[i] = 0;
	for(int i = 256; i < 512; i++)
		new_pml4_pointer[i] = this_pml4_pointer[i];
	
	return PageSpace(new_pml4_page);
}

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
	volatile uint64_t *pml4_pointer = (uint64_t *)physicalToVirtual(p_pml4Address);
	uint64_t pml4_entry = pml4_pointer[pml4_index];

	// find the pdpt entry; create pdpt if necessary
	volatile uint64_t *pdpt_pointer = (uint64_t *)physicalToVirtual(pml4_entry & 0xFFFFFFFFFFFFF000);
	if((pml4_entry & kPagePresent) == 0) {
		// allocate a new pdpt
		uintptr_t pdpt_page = tableAllocator->allocate();
		pdpt_pointer = (uint64_t *)physicalToVirtual(pdpt_page);
		for(int i = 0; i < 512; i++)
			pdpt_pointer[i] = 0;
		pml4_pointer[pml4_index] = pdpt_page | kPagePresent | kPageWrite | kPageUser;
	}
	uint64_t pdpt_entry = pdpt_pointer[pdpt_index];
	
	// find the pd entry; create pd if necessary
	volatile uint64_t *pd_pointer = (uint64_t *)physicalToVirtual(pdpt_entry & 0xFFFFFFFFFFFFF000);
	if((pdpt_entry & kPagePresent) == 0) {
		// allocate a new pd
		uintptr_t pd_page = tableAllocator->allocate();
		pd_pointer = (uint64_t *)physicalToVirtual(pd_page);
		for(int i = 0; i < 512; i++)
			pd_pointer[i] = 0;
		pdpt_pointer[pdpt_index] = pd_page | kPagePresent | kPageWrite | kPageUser;
	}
	uint64_t pd_entry = pd_pointer[pd_index];
	
	// find the pt entry; create pt if necessary
	volatile uint64_t *pt_pointer = (uint64_t *)physicalToVirtual(pd_entry & 0xFFFFFFFFFFFFF000);
	if((pd_entry & kPagePresent) == 0) {
		// allocate a new pt
		uintptr_t pt_page = tableAllocator->allocate();
		pt_pointer = (uint64_t *)physicalToVirtual(pt_page);
		for(int i = 0; i < 512; i++)
			pt_pointer[i] = 0;
		pd_pointer[pd_index] = pt_page | kPagePresent | kPageWrite | kPageUser;
	}
	
	// setup the new pt entry
	if((pt_pointer[pt_index] & kPagePresent) != 0) {
		debug::criticalLogger->log("pk_page_map(): Page already mapped!");
		debug::panic();
	}
	pt_pointer[pt_index] = physical | kPagePresent | kPageWrite | kPageUser;
}

}} // namespace thor::memory

