
#include "../../../frigg/include/types.hpp"
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
	uint64_t new_pml4_page = tableAllocator->allocate(1);
	volatile uint64_t *this_pml4_pointer = (uint64_t *)physicalToVirtual(p_pml4Address);
	volatile uint64_t *new_pml4_pointer = (uint64_t *)physicalToVirtual(new_pml4_page);

	for(int i = 0; i < 256; i++)
		new_pml4_pointer[i] = 0;
	for(int i = 256; i < 512; i++)
		new_pml4_pointer[i] = this_pml4_pointer[i];
	
	return PageSpace(new_pml4_page);
}

void PageSpace::mapSingle4k(void *pointer, uintptr_t physical) {
	ASSERT(((uintptr_t)pointer % 0x1000) == 0);
	ASSERT((physical % 0x1000) == 0);

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
		uintptr_t pdpt_page = tableAllocator->allocate(1);
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
		uintptr_t pd_page = tableAllocator->allocate(1);
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
		uintptr_t pt_page = tableAllocator->allocate(1);
		pt_pointer = (uint64_t *)physicalToVirtual(pt_page);
		for(int i = 0; i < 512; i++)
			pt_pointer[i] = 0;
		pd_pointer[pd_index] = pt_page | kPagePresent | kPageWrite | kPageUser;
	}
	
	// setup the new pt entry
	ASSERT((pt_pointer[pt_index] & kPagePresent) == 0);
	pt_pointer[pt_index] = physical | kPagePresent | kPageWrite | kPageUser;
}

PhysicalAddr PageSpace::unmapSingle4k(VirtualAddr pointer) {
	ASSERT((pointer % 0x1000) == 0);

	int pml4_index = (int)((pointer >> 39) & 0x1FF);
	int pdpt_index = (int)((pointer >> 30) & 0x1FF);
	int pd_index = (int)((pointer >> 21) & 0x1FF);
	int pt_index = (int)((pointer >> 12) & 0x1FF);
	
	// find the pml4_entry
	volatile uint64_t *pml4_pointer = (uint64_t *)physicalToVirtual(p_pml4Address);
	uint64_t pml4_entry = pml4_pointer[pml4_index];

	// find the pdpt entry
	ASSERT((pml4_entry & kPagePresent) != 0);
	volatile uint64_t *pdpt_pointer
			= (uint64_t *)physicalToVirtual(pml4_entry & 0xFFFFFFFFFFFFF000);
	uint64_t pdpt_entry = pdpt_pointer[pdpt_index];
	
	// find the pd entry
	ASSERT((pdpt_entry & kPagePresent) != 0);
	volatile uint64_t *pd_pointer
			= (uint64_t *)physicalToVirtual(pdpt_entry & 0xFFFFFFFFFFFFF000);
	uint64_t pd_entry = pd_pointer[pd_index];
	
	// find the pt entry
	ASSERT((pd_entry & kPagePresent) != 0);
	volatile uint64_t *pt_pointer
			= (uint64_t *)physicalToVirtual(pd_entry & 0xFFFFFFFFFFFFF000);
	
	// change the pt entry
	ASSERT((pt_pointer[pt_index] & kPagePresent) != 0);
	pt_pointer[pt_index] ^= kPagePresent;

	return pt_pointer[pt_index] & 0xFFFFFFFFFFFFF000;
}


}} // namespace thor::memory

