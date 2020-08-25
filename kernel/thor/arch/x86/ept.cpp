#include <thor-internal/arch/ept.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/physical.hpp>

namespace thor::vmx {

Error EptSpace::map(uint64_t guestAddress, uint64_t hostAddress, int flags) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);
	int pml4eIdx = (((guestAddress) >> 39) & 0x1ff);
	int pdpteIdx = (((guestAddress) >> 30) & 0x1ff);
	int pdeIdx   = (((guestAddress) >> 21) & 0x1ff);
	int pteIdx   = (((guestAddress) >> 12) & 0x1ff);

	PageAccessor spaceAccessor{spaceRoot};
	auto pml4e = reinterpret_cast<size_t*>(spaceAccessor.get());

	size_t pageFlags = 0;

	pageFlags |= (1 << EPT_READ);
	if(flags & 1) {
		pageFlags |= (1 << EPT_WRITE);
	}
	if(flags & 2) {
		pageFlags |= (1 << EPT_EXEC);
	}

	size_t* pdpte;
	if(!(pml4e[pml4eIdx] & (1 << EPT_READ))) {
		auto pdpte_ptr = physicalAllocator->allocate(kPageSize);
		if(reinterpret_cast<PhysicalAddr>(pdpte_ptr) == static_cast<PhysicalAddr>(-1)) {
			return Error::noMemory;
		}
		size_t entry = ((pdpte_ptr >> 12) << EPT_PHYSADDR) | pageFlags;
		pml4e[pml4eIdx] = entry;
		PageAccessor pdpteAccessor{pdpte_ptr};
		memset(pdpteAccessor.get(), 0, kPageSize);
		pdpte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	} else {
		PageAccessor pdpteAccessor{(pml4e[pml4eIdx] >> EPT_PHYSADDR) << 12};
		pdpte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}


	size_t* pde;
	if(!(pdpte[pdpteIdx] & (1 << EPT_READ))) {
		auto pdpte_ptr = physicalAllocator->allocate(kPageSize);
		if(reinterpret_cast<PhysicalAddr>(pdpte_ptr) == static_cast<PhysicalAddr>(-1)) {
			return Error::noMemory;
		}
		size_t entry = ((pdpte_ptr >> 12) << EPT_PHYSADDR) | pageFlags;
		pdpte[pdpteIdx] = entry;
		PageAccessor pdpteAccessor{pdpte_ptr};
		memset(pdpteAccessor.get(), 0, kPageSize);
		pde = reinterpret_cast<size_t*>(pdpteAccessor.get());
	} else {
		PageAccessor pdpteAccessor{(pdpte[pdpteIdx] >> EPT_PHYSADDR) << 12};
		pde = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}


	size_t* pte;
	if(!(pde[pdeIdx] & (1 << EPT_READ))) {
		auto pt_ptr = physicalAllocator->allocate(kPageSize);
		if(reinterpret_cast<PhysicalAddr>(pt_ptr) == static_cast<PhysicalAddr>(-1)) {
			return Error::noMemory;
		}
		size_t entry = ((pt_ptr >> 12) << EPT_PHYSADDR) | pageFlags;
		pde[pdeIdx] = entry;
		PageAccessor pdpteAccessor{pt_ptr};
		memset(pdpteAccessor.get(), 0, kPageSize);
		pte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	} else {
		PageAccessor pdpteAccessor{(pde[pdeIdx] >> EPT_PHYSADDR) << 12};
		pte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	auto alloc = hostAddress >> 12;
	size_t entry = (alloc << EPT_PHYSADDR) | pageFlags | (6 << EPT_MEMORY_TYPE) | (1 << EPT_IGNORE_PAT);
	pte[pteIdx] = entry | flags;

	return Error::success;
}

bool EptSpace::isMapped(VirtualAddr guestAddress) {
	int pml4eIdx = (((guestAddress) >> 39) & 0x1ff);
	int pdpteIdx = (((guestAddress) >> 30) & 0x1ff);
	int pdeIdx   = (((guestAddress) >> 21) & 0x1ff);
	int pteIdx   = (((guestAddress) >> 12) & 0x1ff);

	PageAccessor spaceAccessor{spaceRoot};
	size_t* pml4e = reinterpret_cast<size_t*>(spaceAccessor.get());

	size_t* pdpte;
	if(!(pml4e[pml4eIdx] & (1 << EPT_READ))) {
		return false;
	} else {
		PageAccessor pdpteAccessor{(pml4e[pml4eIdx] >> EPT_PHYSADDR) << 12};
		pdpte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	size_t* pde;
	if(!(pdpte[pdpteIdx] & (1 << EPT_READ))) {
		return false;
	} else {
		PageAccessor pdpteAccessor{(pdpte[pdpteIdx] >> EPT_PHYSADDR) << 12};
		pde = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	size_t* pte;
	if(!(pde[pdeIdx] & (1 << EPT_READ))) {
		return false;
	} else {
		PageAccessor pdpteAccessor{(pde[pdeIdx] >> EPT_PHYSADDR) << 12};
		pte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	return ((pte[pteIdx] & EPT_READ));
}

uintptr_t EptSpace::translate(uintptr_t guestAddress) {
	int pml4eIdx = (((guestAddress) >> 39) & 0x1ff);
	int pdpteIdx = (((guestAddress) >> 30) & 0x1ff);
	int pdeIdx   = (((guestAddress) >> 21) & 0x1ff);
	int pteIdx   = (((guestAddress) >> 12) & 0x1ff);
	int offset = (size_t)guestAddress & (0x1000 - 1);

	PageAccessor spaceAccessor{spaceRoot};
	size_t* pml4e = reinterpret_cast<size_t*>(spaceAccessor.get());

	size_t* pdpte;
	if(!(pml4e[pml4eIdx] & (1 << EPT_READ))) {
		return -1;
	} else {
		PageAccessor pdpteAccessor{(pml4e[pml4eIdx] >> EPT_PHYSADDR) << 12};
		pdpte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	size_t* pde;
	if(!(pdpte[pdpteIdx] & (1 << EPT_READ))) {
		return -1;
	} else {
		PageAccessor pdpteAccessor{(pdpte[pdpteIdx] >> EPT_PHYSADDR) << 12};
		pde = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	size_t* pte;
	if(!(pde[pdeIdx] & (1 << EPT_READ))) {
		return -1;
	} else {
		PageAccessor pdpteAccessor{(pde[pdeIdx] >> EPT_PHYSADDR) << 12};
		pte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	return ((pte[pteIdx] >> EPT_PHYSADDR) << 12) + offset;
}

PageStatus EptSpace::unmap(uint64_t guestAddress) {
	int pml4eIdx = (((guestAddress) >> 39) & 0x1ff);
	int pdpteIdx = (((guestAddress) >> 30) & 0x1ff);
	int pdeIdx   = (((guestAddress) >> 21) & 0x1ff);
	int pteIdx   = (((guestAddress) >> 12) & 0x1ff);

	PageAccessor spaceAccessor{spaceRoot};
	size_t* pml4e = reinterpret_cast<size_t*>(spaceAccessor.get());

	size_t* pdpte;
	if(!(pml4e[pml4eIdx] & (1 << EPT_READ))) {
		return 0;
	} else {
		PageAccessor pdpteAccessor{(pml4e[pml4eIdx] >> EPT_PHYSADDR) << 12};
		pdpte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	size_t* pde;
	if(!(pdpte[pdpteIdx] & (1 << EPT_READ))) {
		return 0;
	} else {
		PageAccessor pdpteAccessor{(pdpte[pdpteIdx] >> EPT_PHYSADDR) << 12};
		pde = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	size_t* pte;
	if(!(pde[pdeIdx] & (1 << EPT_READ))) {
		return 0;
	} else {
		PageAccessor pdpteAccessor{(pde[pdeIdx] >> EPT_PHYSADDR) << 12};
		pte = reinterpret_cast<size_t*>(pdpteAccessor.get());
	}

	PageStatus status = page_status::present;
	if(pte[pteIdx] & (1 << 9)) {
		status |= page_status::dirty;
	}
	pte[pteIdx] = 0;
	return status;
}

Error EptSpace::store(uintptr_t guestAddress, size_t size, const void* buffer) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);
	size_t progress = 0;
	while(progress < size) {
		VirtualAddr write = guestAddress + progress;
		size_t misalign = (VirtualAddr)write % kPageSize;
		size_t chunk = frigg::min(kPageSize - misalign, size - progress);

		PhysicalAddr page = translate(write - misalign);
		if(page == PhysicalAddr(-1)) {
			return Error::fault;
		}

		PageAccessor accessor{page};
		memcpy((char *)accessor.get() + misalign, (char *)buffer + progress, chunk);
		progress += chunk;
	}
	return Error::success;
}

Error EptSpace::load(uintptr_t guestAddress, size_t size, void* buffer) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);
	size_t progress = 0;
	while(progress < size) {
		VirtualAddr write = guestAddress + progress;
		size_t misalign = (VirtualAddr)write % kPageSize;
		size_t chunk = frigg::min(kPageSize - misalign, size - progress);

		PhysicalAddr page = translate(write - misalign);
		if(page == PhysicalAddr(-1)) {
			return Error::fault;
		}

		PageAccessor accessor{page};
		memcpy((char *)buffer + progress, (char *)accessor.get() + misalign, chunk);
		progress += chunk;
	}
	return Error::success;
}

bool EptSpace::submitShootdown(ShootNode *node) {
	EptPtr ptr = {spaceRoot, 0};
	asm volatile (
		"invept (%0), %1;"
		: : "r"(&ptr), "r"((uint64_t)1)
	);
	node->complete();
	return false;
}

void EptSpace::retire(RetireNode *node) {
	EptPtr ptr = {spaceRoot, 0};
	asm volatile (
		"invept (%0), %1;"
		: : "r"(&ptr), "r"((uint64_t)1)
	);
	node->complete();
}

EptSpace::~EptSpace() {
	PageAccessor spaceAccessor{spaceRoot};
	auto pml4e = reinterpret_cast<size_t*>(spaceAccessor.get());

	for(int i = 0; i < 512; i++) {
		if(pml4e[i] & (1 << EPT_READ)) {
			PageAccessor pdpteAccessor{(pml4e[i] >> EPT_PHYSADDR) << 12};
			auto pdpte = reinterpret_cast<size_t*>(pdpteAccessor.get());
			for(int j = 0; j < 512; j++) {
				if(pdpte[j] & (1 << EPT_READ)) {
					PageAccessor pdeAccessor{(pdpte[j] >> EPT_PHYSADDR) << 12};
					auto pde = reinterpret_cast<size_t*>(pdeAccessor.get());
					for(int k = 0; k < 512; k++) {
						if(pde[k] & (1 << EPT_READ)) {
							PageAccessor ptAccessor{(pde[k] >> EPT_PHYSADDR) << 12};
							auto pte = reinterpret_cast<size_t*>(ptAccessor.get());
							for(int l = 0; l < 512; l++) {
								if(pte[l] & (1 << EPT_READ)) {
									physicalAllocator->free((size_t)((pte[l] >> EPT_PHYSADDR)) << 12, kPageSize);
								}
							}
							physicalAllocator->free((size_t)(pde[k] >> EPT_PHYSADDR) << 12, kPageSize);
						}
					}
					physicalAllocator->free((size_t)(pdpte[j] >> EPT_PHYSADDR) << 12, kPageSize);
				}
			}
			physicalAllocator->free((size_t)(pml4e[i] >> EPT_PHYSADDR) << 12, kPageSize);
		}
	}
	physicalAllocator->free(spaceRoot, kPageSize);
}

} // namespace thor
