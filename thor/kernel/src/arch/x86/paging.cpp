
#include "generic/kernel.hpp"

enum {
	kPagePresent = 0x1,
	kPageWrite = 0x2,
	kPageUser = 0x4,
	kPageXd = 0x8000000000000000
};

namespace thor {

void invlpg(const void *address) {
	auto p = reinterpret_cast<const char *>(address);
	asm volatile ("invlpg %0" : : "m"(*p));
}

constexpr PhysicalWindow::PhysicalWindow(uint64_t *table, void *content)
: _table{table}, _content{content}, _locked{} { }

void *PhysicalWindow::acquire(PhysicalAddr physical) {
	assert(!(physical % kPageSize));

	// FIXME: This hack is uncredibly ugly!
	// Fix global initializers instead of using this hack!
	_table = reinterpret_cast<uint64_t *>(0xFFFF'FF80'0000'2000);
	_content = reinterpret_cast<uint64_t *>(0xFFFF'FF80'0040'0000);

	for(int i = 0; i < 512; ++i) {
		if(_locked[i])
			continue;

//		frigg::infoLogger() << "Locking " << i << " to " << (void *)physical << frigg::endLog;
		_locked[i] = true;
		_table[i] = physical | kPagePresent | kPageWrite | kPageXd;
		invlpg((char *)_content + i * kPageSize);
		return (char *)_content + i * kPageSize;
	}

	frigg::panicLogger() << "Cannot lock a slot of a PhysicalWindow" << frigg::endLog;
	__builtin_unreachable();
}

void PhysicalWindow::release(void *pointer) {
	assert(!(uintptr_t(pointer) % kPageSize));
	assert((char *)pointer >= (char *)_content);
	auto index = ((char *)pointer - (char *)_content) / kPageSize;

	_locked[index] = false;
	_table[index] = 0;
	invlpg((char *)_content + index * kPageSize);
}

PhysicalWindow generalWindow{reinterpret_cast<uint64_t *>(0xFFFF'FF80'0000'2000),
		reinterpret_cast<void *>(0xFFFF'FF80'0040'0000)};

// --------------------------------------------------------
// Kernel paging management.
// --------------------------------------------------------

frigg::LazyInitializer<KernelPageSpace> kernelSpaceSingleton;

void KernelPageSpace::initialize(PhysicalAddr pml4_address) {
	kernelSpaceSingleton.initialize(pml4_address);
}

KernelPageSpace &KernelPageSpace::global() {
	return *kernelSpaceSingleton;
}

KernelPageSpace::KernelPageSpace(PhysicalAddr pml4_address)
: _pml4Address{pml4_address} { }

void KernelPageSpace::mapSingle4k(VirtualAddr pointer, PhysicalAddr physical, uint32_t flags) {
	assert((pointer % 0x1000) == 0);
	assert((physical % 0x1000) == 0);
	
	auto &region = SkeletalRegion::global();

	int pml4_index = (int)((pointer >> 39) & 0x1FF);
	int pdpt_index = (int)((pointer >> 30) & 0x1FF);
	int pd_index = (int)((pointer >> 21) & 0x1FF);
	int pt_index = (int)((pointer >> 12) & 0x1FF);

	// the pml4 exists already
	uint64_t *pml4_pointer = (uint64_t *)region.access(_pml4Address);

	// make sure there is a pdpt
	uint64_t pml4_initial_entry = pml4_pointer[pml4_index];
	uint64_t *pdpt_pointer;
	if((pml4_initial_entry & kPagePresent) != 0) {
		pdpt_pointer = (uint64_t *)region.access(pml4_initial_entry & 0x000FFFFFFFFFF000);
	}else{
		PhysicalAddr pdpt_page = SkeletalRegion::global().allocate();

		pdpt_pointer = (uint64_t *)region.access(pdpt_page);
		for(int i = 0; i < 512; i++)
			pdpt_pointer[i] = 0;
		
		uint64_t new_entry = pdpt_page | kPagePresent | kPageWrite;
		pml4_pointer[pml4_index] = new_entry;
	}
	assert(!(pml4_pointer[pml4_index] & kPageUser));
	
	// make sure there is a pd
	uint64_t pdpt_initial_entry = pdpt_pointer[pdpt_index];
	uint64_t *pd_pointer;
	if((pdpt_initial_entry & kPagePresent) != 0) {
		pd_pointer = (uint64_t *)region.access(pdpt_initial_entry & 0x000FFFFFFFFFF000);
	}else{
		PhysicalAddr pd_page = SkeletalRegion::global().allocate();

		pd_pointer = (uint64_t *)region.access(pd_page);
		for(int i = 0; i < 512; i++)
			pd_pointer[i] = 0;
		
		uint64_t new_entry = pd_page | kPagePresent | kPageWrite;
		pdpt_pointer[pdpt_index] = new_entry;
	}
	assert(!(pdpt_pointer[pdpt_index] & kPageUser));
	
	// make sure there is a pt
	uint64_t pd_initial_entry = pd_pointer[pd_index];
	uint64_t *pt_pointer;
	if((pd_initial_entry & kPagePresent) != 0) {
		pt_pointer = (uint64_t *)region.access(pd_initial_entry & 0x000FFFFFFFFFF000);
	}else{
		PhysicalAddr pt_page = SkeletalRegion::global().allocate();

		pt_pointer = (uint64_t *)region.access(pt_page);
		for(int i = 0; i < 512; i++)
			pt_pointer[i] = 0;
		
		uint64_t new_entry = pt_page | kPagePresent | kPageWrite;
		pd_pointer[pd_index] = new_entry;
	}
	assert(!(pd_pointer[pd_index] & kPageUser));

	// setup the new pt entry
	assert((pt_pointer[pt_index] & kPagePresent) == 0);
	uint64_t new_entry = physical | kPagePresent;
	if(flags & page_access::write)
		new_entry |= kPageWrite;
	if(!(flags & page_access::execute))
		new_entry |= kPageXd;
	pt_pointer[pt_index] = new_entry;
}

PhysicalAddr KernelPageSpace::unmapSingle4k(VirtualAddr pointer) {
	assert((pointer % 0x1000) == 0);

	auto &region = SkeletalRegion::global();

	int pml4_index = (int)((pointer >> 39) & 0x1FF);
	int pdpt_index = (int)((pointer >> 30) & 0x1FF);
	int pd_index = (int)((pointer >> 21) & 0x1FF);
	int pt_index = (int)((pointer >> 12) & 0x1FF);
	
	// find the pml4_entry
	uint64_t *pml4_pointer = (uint64_t *)region.access(_pml4Address);
	uint64_t pml4_entry = pml4_pointer[pml4_index];

	// find the pdpt entry
	assert((pml4_entry & kPagePresent) != 0);
	uint64_t *pdpt_pointer = (uint64_t *)region.access(pml4_entry & 0x000FFFFFFFFFF000);
	uint64_t pdpt_entry = pdpt_pointer[pdpt_index];
	
	// find the pd entry
	assert((pdpt_entry & kPagePresent) != 0);
	uint64_t *pd_pointer = (uint64_t *)region.access(pdpt_entry & 0x000FFFFFFFFFF000);
	uint64_t pd_entry = pd_pointer[pd_index];
	
	// find the pt entry
	assert((pd_entry & kPagePresent) != 0);
	uint64_t *pt_pointer = (uint64_t *)region.access(pd_entry & 0x000FFFFFFFFFF000);
	
	// change the pt entry
	assert((pt_pointer[pt_index] & kPagePresent) != 0);
	pt_pointer[pt_index] ^= kPagePresent;
	return pt_pointer[pt_index] & 0x000FFFFFFFFFF000;
}

PhysicalAddr KernelPageSpace::getPml4() {
	return _pml4Address;
}


// --------------------------------------------------------
// ClientPageSpace
// --------------------------------------------------------

ClientPageSpace::ClientPageSpace() {
	_pml4Address = physicalAllocator->allocate(kPageSize);
	_window = KernelVirtualMemory::global().allocate(0x200000);
	for(size_t i = 0; i < 512; i++)
		_tileLocks[i] = false;

	// Initialize the bottom half to unmapped memory.
	TableAccessor accessor{this};
	accessor.aim(_pml4Address);

	for(size_t i = 0; i < 256; i++)
		accessor[i] = 0;

	// Share the top half with the kernel.
	auto kernel_pml4 = KernelPageSpace::global().getPml4();
	auto kernel_table = (uint64_t *)SkeletalRegion::global().access(kernel_pml4);

	for(size_t i = 256; i < 512; i++) {
		assert(kernel_table[i] & kPagePresent);
		accessor[i] = kernel_table[i];
	}
}

ClientPageSpace::~ClientPageSpace() {
	assert(!"Implement this");
}

void ClientPageSpace::activate() {
	asm volatile ( "mov %0, %%cr3" : : "r"( _pml4Address ) : "memory" );
}

void ClientPageSpace::mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
		bool user_page, uint32_t flags) {
	assert((pointer % 0x1000) == 0);
	assert((physical % 0x1000) == 0);
	
	TableAccessor accessor4{this};
	TableAccessor accessor3{this};
	TableAccessor accessor2{this};
	TableAccessor accessor1{this};

	auto index4 = (int)((pointer >> 39) & 0x1FF);
	auto index3 = (int)((pointer >> 30) & 0x1FF);
	auto index2 = (int)((pointer >> 21) & 0x1FF);
	auto index1 = (int)((pointer >> 12) & 0x1FF);

	// The PML4 does always exist.
	accessor4.aim(_pml4Address);

	// Make sure there is a PDPT.
	if(accessor4[index4] & kPagePresent) {
		accessor3.aim(accessor4[index4] & 0x000FFFFFFFFFF000);
	}else{
		auto tbl_address = SkeletalRegion::global().allocate();
		accessor3.aim(tbl_address);
		for(int i = 0; i < 512; i++)
			accessor3[i] = 0;
		
		uint64_t new_entry = tbl_address | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		accessor4[index4] = new_entry;
	}
	assert(user_page ? ((accessor4[index4] & kPageUser) != 0)
			: ((accessor4[index4] & kPageUser) == 0));
	
	// Make sure there is a PD.
	if(accessor3[index3] & kPagePresent) {
		accessor2.aim(accessor3[index3] & 0x000FFFFFFFFFF000);
	}else{
		auto tbl_address = SkeletalRegion::global().allocate();
		accessor2.aim(tbl_address);
		for(int i = 0; i < 512; i++)
			accessor2[i] = 0;
		
		uint64_t new_entry = tbl_address | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		accessor3[index3] = new_entry;
	}
	assert(user_page ? ((accessor3[index3] & kPageUser) != 0)
			: ((accessor3[index3] & kPageUser) == 0));
	
	// Make sure there is a PT.
	if(accessor2[index2] & kPagePresent) {
		accessor1.aim(accessor2[index2] & 0x000FFFFFFFFFF000);
	}else{
		auto tbl_address = SkeletalRegion::global().allocate();
		accessor1.aim(tbl_address);
		for(int i = 0; i < 512; i++)
			accessor1[i] = 0;
		
		uint64_t new_entry = tbl_address | kPagePresent | kPageWrite;
		if(user_page)
			new_entry |= kPageUser;
		accessor2[index2] = new_entry;
	}
	assert(user_page ? ((accessor2[index2] & kPageUser) != 0)
			: ((accessor2[index2] & kPageUser) == 0));

	// Setup the new PTE.
	assert(!(accessor1[index1] & kPagePresent));
	uint64_t new_entry = physical | kPagePresent;
	if(user_page)
		new_entry |= kPageUser;
	if(flags & page_access::write)
		new_entry |= kPageWrite;
	if(!(flags & page_access::execute))
		new_entry |= kPageXd;
	accessor1[index1] = new_entry;
}

PhysicalAddr ClientPageSpace::unmapSingle4k(VirtualAddr pointer) {
	assert(!(pointer & (kPageSize - 1)));

	TableAccessor accessor4{this};
	TableAccessor accessor3{this};
	TableAccessor accessor2{this};
	TableAccessor accessor1{this};

	auto index4 = (int)((pointer >> 39) & 0x1FF);
	auto index3 = (int)((pointer >> 30) & 0x1FF);
	auto index2 = (int)((pointer >> 21) & 0x1FF);
	auto index1 = (int)((pointer >> 12) & 0x1FF);
	
	// The PML4 is always present.
	accessor4.aim(_pml4Address);

	// Find the PDPT.
	assert(accessor4[index4] & kPagePresent);
	accessor3.aim(accessor4[index4] & 0x000FFFFFFFFFF000);
	
	// Find the PD.
	assert(accessor3[index3] & kPagePresent);
	accessor2.aim(accessor3[index3] & 0x000FFFFFFFFFF000);
	
	// Find the PT.
	assert(accessor2[index2] & kPagePresent);
	accessor1.aim(accessor2[index2] & 0x000FFFFFFFFFF000);
	
	assert(accessor1[index1] & kPagePresent);
	accessor1[index1] &= ~uint64_t{kPagePresent};
	return accessor1[index1] & 0x000FFFFFFFFFF000;
}

bool ClientPageSpace::isMapped(VirtualAddr pointer) {
	assert(!(pointer & (kPageSize - 1)));

	TableAccessor accessor4{this};
	TableAccessor accessor3{this};
	TableAccessor accessor2{this};
	TableAccessor accessor1{this};

	auto index4 = (int)((pointer >> 39) & 0x1FF);
	auto index3 = (int)((pointer >> 30) & 0x1FF);
	auto index2 = (int)((pointer >> 21) & 0x1FF);
	auto index1 = (int)((pointer >> 12) & 0x1FF);
	
	// The PML4 is always present.
	accessor4.aim(_pml4Address);

	// Find the PDPT.
	if(!(accessor4[index4] & kPagePresent))
		return false;
	accessor3.aim(accessor4[index4] & 0x000FFFFFFFFFF000);
	
	// Find the PD.
	if(!(accessor3[index3] & kPagePresent))
		return false;
	accessor2.aim(accessor3[index3] & 0x000FFFFFFFFFF000);
	
	// Find the PT.
	if(!(accessor2[index2] & kPagePresent))
		return false;
	accessor1.aim(accessor2[index2] & 0x000FFFFFFFFFF000);
	
	return accessor1[index1] & kPagePresent;
}

ClientPageSpace::TableAccessor::~TableAccessor() {
	if(_tile == -1)
		return;
	
	_space->_tileLocks[_tile] = false;
	// TODO: This is not really necessary; we could do a remap instead.
	KernelPageSpace::global().unmapSingle4k(reinterpret_cast<VirtualAddr>(_space->_window)
			+ _tile * kPageSize);
}

void ClientPageSpace::TableAccessor::aim(PhysicalAddr address) {
	for(size_t i = 0; i < 512; ++i) {
		if(_space->_tileLocks[i])
			continue;

		_tile = i;
		_space->_tileLocks[_tile] = true;
		auto ptr = reinterpret_cast<VirtualAddr>(_space->_window) + _tile * kPageSize;
		KernelPageSpace::global().mapSingle4k(ptr, address, page_access::write);
		invlpg(reinterpret_cast<void *>(ptr));
		return;
	}

	assert(!"Fix this");
}

uint64_t &ClientPageSpace::TableAccessor::operator[] (size_t n) {
	assert(_tile != -1);
	return reinterpret_cast<uint64_t *>(reinterpret_cast<VirtualAddr>(_space->_window)
			+ _tile * kPageSize)[n];
}

void thorRtInvalidateSpace() {
	asm volatile ("movq %%cr3, %%rax\n\t"
		"movq %%rax, %%cr3" : : : "%rax", "memory");
}

} // namespace thor

