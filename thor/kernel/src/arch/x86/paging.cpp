
#include "generic/kernel.hpp"

enum {
	kPagePresent = 0x1,
	kPageWrite = 0x2,
	kPageUser = 0x4,
	kPageXd = 0x8000000000000000
};

namespace thor {

frigg::LazyInitializer<PageSpace> kernelSpace;

void *physicalToVirtual(PhysicalAddr address) {
	return (void *)(0xFFFF800100000000 + address);
}

// --------------------------------------------------------
// PageSpace
// --------------------------------------------------------

PageSpace::PageSpace(PhysicalAddr pml4_address)
			: p_pml4Address(pml4_address) { }

void PageSpace::activate() {
	asm volatile ( "mov %0, %%cr3" : : "r"( p_pml4Address ) : "memory" );
}

PageSpace PageSpace::cloneFromKernelSpace() {
	PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
	PhysicalAddr new_pml4_page = physicalAllocator->allocate(physical_guard, 0x1000);
	physical_guard.unlock();

	uint64_t *this_pml4_pointer = (uint64_t *)physicalToVirtual(p_pml4Address);
	uint64_t *new_pml4_pointer = (uint64_t *)physicalToVirtual(new_pml4_page);

	for(int i = 0; i < 256; i++)
		new_pml4_pointer[i] = 0;
	for(int i = 256; i < 512; i++) {
		assert((this_pml4_pointer[i] & kPagePresent) != 0);
		new_pml4_pointer[i] = this_pml4_pointer[i];
	}

	return PageSpace(new_pml4_page);
}

void invlpg(const void *address) {
	auto p = reinterpret_cast<const char *>(address);
	asm volatile ("invlpg %0" : : "m"(*p));
}

// provides a window into physical memory.
// note that this struct is stateless in the sense that
// calling access() twice may invalidate the pointer returned
// by the first call!
struct StatelessWindow {
	StatelessWindow(VirtualAddr directory, int index, int offset, int size)
	: _directory(directory), _index(index), _offset(offset),
			_mask((1 << __builtin_ctz(size)) - 1) {
		assert(_index);
		assert(size == (1 << __builtin_ctz(size)));
	}

	void *access(PhysicalAddr physical) {
		assert(!(physical % kPageSize));

		// this hash tries to avoid mapping all large pages to the same value.
		int h = physical >> 12;
		h ^= physical >> 21;
		h ^= physical >> 30;
		h = _offset + (h & _mask);

		auto pt = reinterpret_cast<uint64_t *>(_directory + _index * kPageSize);
		auto virt = reinterpret_cast<void *>(_directory
				+ _index * 512 * kPageSize + h * kPageSize);
		if((pt[h] & kPagePresent) && ((pt[h] & 0x000FFFFFFFFFF000) == physical))
			return virt;

		pt[h] = physical | kPagePresent | kPageWrite | kPageXd;
		invlpg(virt);
		return virt;
	}

private:
	VirtualAddr _directory;
	int _index;
	int _offset;
	int _mask;
};

void PageSpace::mapSingle4k(PhysicalChunkAllocator::Guard &physical_guard,
		VirtualAddr pointer, PhysicalAddr physical, bool user_page, uint32_t flags) {
	assert((pointer % 0x1000) == 0);
	assert((physical % 0x1000) == 0);

	int pml4_index = (int)((pointer >> 39) & 0x1FF);
	int pdpt_index = (int)((pointer >> 30) & 0x1FF);
	int pd_index = (int)((pointer >> 21) & 0x1FF);
	int pt_index = (int)((pointer >> 12) & 0x1FF);

	StatelessWindow window1(0xFFFF'FF80'0000'0000, 1, 0, 128);
	StatelessWindow window2(0xFFFF'FF80'0000'0000, 1, 128, 64);
	StatelessWindow window3(0xFFFF'FF80'0000'0000, 1, 192, 32);
	StatelessWindow window4(0xFFFF'FF80'0000'0000, 1, 224, 32);

	// the pml4 exists already
	uint64_t *pml4_pointer = (uint64_t *)window4.access(p_pml4Address);

	// make sure there is a pdpt
	uint64_t pml4_initial_entry = pml4_pointer[pml4_index];
	uint64_t *pdpt_pointer;
	if((pml4_initial_entry & kPagePresent) != 0) {
		pdpt_pointer = (uint64_t *)window3.access(pml4_initial_entry & 0x000FFFFFFFFFF000);
	}else{
		if(!physical_guard.isLocked())
			physical_guard.lock();
		PhysicalAddr pdpt_page = physicalAllocator->allocate(physical_guard, 0x1000);

		pdpt_pointer = (uint64_t *)window3.access(pdpt_page);
		for(int i = 0; i < 512; i++)
			pdpt_pointer[i] = 0;
		
		uint64_t new_entry = pdpt_page | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		pml4_pointer[pml4_index] = new_entry;
	}
	assert(user_page ? ((pml4_pointer[pml4_index] & kPageUser) != 0)
			: ((pml4_pointer[pml4_index] & kPageUser) == 0));
	
	// make sure there is a pd
	uint64_t pdpt_initial_entry = pdpt_pointer[pdpt_index];
	uint64_t *pd_pointer;
	if((pdpt_initial_entry & kPagePresent) != 0) {
		pd_pointer = (uint64_t *)window2.access(pdpt_initial_entry & 0x000FFFFFFFFFF000);
	}else{
		if(!physical_guard.isLocked())
			physical_guard.lock();
		PhysicalAddr pd_page = physicalAllocator->allocate(physical_guard, 0x1000);

		pd_pointer = (uint64_t *)window2.access(pd_page);
		for(int i = 0; i < 512; i++)
			pd_pointer[i] = 0;
		
		uint64_t new_entry = pd_page | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		pdpt_pointer[pdpt_index] = new_entry;
	}
	assert(user_page ? ((pdpt_pointer[pdpt_index] & kPageUser) != 0)
			: ((pdpt_pointer[pdpt_index] & kPageUser) == 0));
	
	// make sure there is a pt
	uint64_t pd_initial_entry = pd_pointer[pd_index];
	uint64_t *pt_pointer;
	if((pd_initial_entry & kPagePresent) != 0) {
		pt_pointer = (uint64_t *)window1.access(pd_initial_entry & 0x000FFFFFFFFFF000);
	}else{
		if(!physical_guard.isLocked())
			physical_guard.lock();
		PhysicalAddr pt_page = physicalAllocator->allocate(physical_guard, 0x1000);

		pt_pointer = (uint64_t *)window1.access(pt_page);
		for(int i = 0; i < 512; i++)
			pt_pointer[i] = 0;
		
		uint64_t new_entry = pt_page | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		pd_pointer[pd_index] = new_entry;
	}
	assert(user_page ? ((pd_pointer[pd_index] & kPageUser) != 0)
			: ((pd_pointer[pd_index] & kPageUser) == 0));

	// setup the new pt entry
	assert((pt_pointer[pt_index] & kPagePresent) == 0);
	uint64_t new_entry = physical | kPagePresent;
	if(user_page)
		new_entry |= kPageUser;
	if((flags & kAccessWrite) != 0)
		new_entry |= kPageWrite;
	if((flags & kAccessExecute) == 0)
		new_entry |= kPageXd;
	pt_pointer[pt_index] = new_entry;

	frigg::barrier();
}

PhysicalAddr PageSpace::unmapSingle4k(VirtualAddr pointer) {
	assert((pointer % 0x1000) == 0);

	int pml4_index = (int)((pointer >> 39) & 0x1FF);
	int pdpt_index = (int)((pointer >> 30) & 0x1FF);
	int pd_index = (int)((pointer >> 21) & 0x1FF);
	int pt_index = (int)((pointer >> 12) & 0x1FF);
	
	// find the pml4_entry
	uint64_t *pml4_pointer = (uint64_t *)physicalToVirtual(p_pml4Address);
	uint64_t pml4_entry = pml4_pointer[pml4_index];

	// find the pdpt entry
	assert((pml4_entry & kPagePresent) != 0);
	uint64_t *pdpt_pointer = (uint64_t *)physicalToVirtual(pml4_entry & 0x000FFFFFFFFFF000);
	uint64_t pdpt_entry = pdpt_pointer[pdpt_index];
	
	// find the pd entry
	assert((pdpt_entry & kPagePresent) != 0);
	uint64_t *pd_pointer = (uint64_t *)physicalToVirtual(pdpt_entry & 0x000FFFFFFFFFF000);
	uint64_t pd_entry = pd_pointer[pd_index];
	
	// find the pt entry
	assert((pd_entry & kPagePresent) != 0);
	uint64_t *pt_pointer = (uint64_t *)physicalToVirtual(pd_entry & 0x000FFFFFFFFFF000);
	
	// change the pt entry
	assert((pt_pointer[pt_index] & kPagePresent) != 0);
	pt_pointer[pt_index] ^= kPagePresent;

	frigg::barrier();

	return pt_pointer[pt_index] & 0x000FFFFFFFFFF000;
}

bool PageSpace::isMapped(VirtualAddr pointer) {
	assert((pointer % 0x1000) == 0);

	int pml4_index = (int)((pointer >> 39) & 0x1FF);
	int pdpt_index = (int)((pointer >> 30) & 0x1FF);
	int pd_index = (int)((pointer >> 21) & 0x1FF);
	int pt_index = (int)((pointer >> 12) & 0x1FF);
	
	// check the pml4_entry
	uint64_t *pml4_pointer = (uint64_t *)physicalToVirtual(p_pml4Address);
	uint64_t pml4_entry = pml4_pointer[pml4_index];

	// check the pdpt entry
	if(!(pml4_entry & kPagePresent))
		return false;
	uint64_t *pdpt_pointer = (uint64_t *)physicalToVirtual(pml4_entry & 0x000FFFFFFFFFF000);
	uint64_t pdpt_entry = pdpt_pointer[pdpt_index];
	
	// check the pd entry
	if(!(pdpt_entry & kPagePresent))
		return false;
	uint64_t *pd_pointer = (uint64_t *)physicalToVirtual(pdpt_entry & 0x000FFFFFFFFFF000);
	uint64_t pd_entry = pd_pointer[pd_index];
	
	// check the pt entry
	if(!(pd_entry & kPagePresent))
		return false;
	uint64_t *pt_pointer = (uint64_t *)physicalToVirtual(pd_entry & 0x000FFFFFFFFFF000);
	
	// check the pt entry
	if(!(pt_pointer[pt_index] & kPagePresent))
		return false;
	return true;
}

PhysicalAddr PageSpace::getPml4() {
	return p_pml4Address;
}

void thorRtInvalidateSpace() {
	asm volatile ("movq %%cr3, %%rax\n\t"
		"movq %%rax, %%cr3" : : : "%rax", "memory");
}

} // namespace thor

