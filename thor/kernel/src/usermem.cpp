
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// Memory
// --------------------------------------------------------

Memory::Memory(Type type)
: flags(0), loadState(*kernelAlloc), processQueue(*kernelAlloc), loadQueue(*kernelAlloc),
		lockQueue(*kernelAlloc), waitQueue(*kernelAlloc),
		p_type(type), p_physicalPages(*kernelAlloc) { }

Memory::~Memory() {
	if(p_type == kTypePhysical) {
		// do nothing
	}else if(p_type == kTypeAllocated || p_type == kTypeCopyOnWrite) {
		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
		for(size_t i = 0; i < p_physicalPages.size(); i++) {
			if(p_physicalPages[i] != PhysicalAddr(-1))
				physicalAllocator->free(physical_guard, p_physicalPages[i]);
		}
		physical_guard.unlock();
	}else{
		assert(!"Illegal memory type");
	}
}

auto Memory::getType() -> Type {
	return p_type;
}

void Memory::resize(size_t num_pages) {
	assert(p_physicalPages.size() < num_pages);
	p_physicalPages.resize(num_pages, -1);
}

void Memory::setPageAt(size_t offset, PhysicalAddr page) {
	p_physicalPages[offset / kPageSize] = page;
}

PhysicalAddr Memory::getPageAt(size_t offset) {
	assert(offset % kPageSize == 0);
	assert(offset / kPageSize < p_physicalPages.size());

	return p_physicalPages[offset / kPageSize];
}

PhysicalAddr Memory::resolveOriginalAt(size_t offset) {
	assert(offset % kPageSize == 0);

	if(p_type == Memory::kTypeAllocated) {
		PhysicalAddr page = p_physicalPages[offset / kPageSize];
		assert(page != PhysicalAddr(-1));
		return page;
	}else if(p_type == Memory::kTypeOnDemand) {
		return p_physicalPages[offset / kPageSize];
	}else if(p_type == Memory::kTypeCopyOnWrite) {
		PhysicalAddr page = p_physicalPages[offset / kPageSize];
		if(page != PhysicalAddr(-1))
			return page;
		return master->resolveOriginalAt(offset);
	}else{
		assert(!"Unexpected memory type");
		__builtin_unreachable();
	}
}

size_t Memory::numPages() {
	return p_physicalPages.size();
}

void Memory::zeroPages() {
	assert(p_type == kTypeAllocated);
	
	for(size_t i = 0; i < p_physicalPages.size(); i++) {
		PhysicalAddr page = p_physicalPages[i];
		assert(page != PhysicalAddr(-1));
		memset(physicalToVirtual(page), 0, kPageSize);
	}
}

void Memory::copyTo(size_t offset, void *source, size_t length) {
	assert(p_type == kTypeAllocated);
	
	size_t disp = 0;
	size_t index = offset / kPageSize;
	
	size_t misalign = offset % kPageSize;
	if(misalign > 0) {
		size_t prefix = frigg::min(kPageSize - misalign, length);
		PhysicalAddr page = p_physicalPages[index];
		assert(page != PhysicalAddr(-1));
		memcpy((uint8_t *)physicalToVirtual(page) + misalign, source, prefix);
		disp += prefix;
		index++;
	}

	while(length - disp >= kPageSize) {
		assert(((offset + disp) % kPageSize) == 0);
		PhysicalAddr page = p_physicalPages[index];
		assert(page != PhysicalAddr(-1));
		memcpy(physicalToVirtual(page), (uint8_t *)source + disp, kPageSize);
		disp += kPageSize;
		index++;
	}

	if(length - disp > 0) {
		PhysicalAddr page = p_physicalPages[index];
		assert(page != PhysicalAddr(-1));
		memcpy(physicalToVirtual(page), (uint8_t *)source + disp, length - disp);
	}
}

void Memory::loadMemory(uintptr_t offset, size_t size) {
	assert(size > 0);
	assert(offset % kPageSize == 0);
	assert(size % kPageSize == 0);

	uintptr_t chunk_offset = 0;
	while(chunk_offset < size) {
		if(loadState[(offset + chunk_offset) / kPageSize] == kStateMissing) {
			size_t chunk_size;
			for(chunk_size = 0; chunk_offset + chunk_size < size; chunk_size += kPageSize) {
				uintptr_t page_offset = offset + chunk_offset + chunk_size;
				uintptr_t page_index = page_offset / kPageSize;
				if(loadState[page_index] != kStateMissing)
					break;

				// allocate a new page
				assert(p_physicalPages[page_index] == PhysicalAddr(-1));

				PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
				PhysicalAddr physical = physicalAllocator->allocate(physical_guard, kPageSize);
				physical_guard.unlock();

				memset(physicalToVirtual(physical), 0, kPageSize);
				p_physicalPages[page_index] = physical;

				// mark the page as loading
				loadState[page_index] = kStateLoading;
			}
			
			// submit a load request for the page
//			infoLogger->log() << "LoadOrder(" << (offset + chunk_offset) << ", " << chunk_size
//					<< ")" << frigg::EndLog();
			LoadOrder load_order(offset + chunk_offset, chunk_size);
			if(!processQueue.empty()) {
				ProcessRequest process_request = processQueue.removeFront();
				performLoad(&process_request, &load_order);
			}else{
				loadQueue.addBack(load_order);
			}

			chunk_offset += chunk_size;
		}else if(loadState[(offset + chunk_offset) / kPageSize] == kStateLoading
				|| loadState[(offset + chunk_offset) / kPageSize] == kStateLoaded) {
			chunk_offset += kPageSize;
		}else{
			frigg::panicLogger.log() << "Illegal LoadState" << frigg::EndLog();
		}
	}
}

void Memory::performLoad(ProcessRequest *process_request, LoadOrder *load_order) {
	UserEvent user_event(UserEvent::kTypeMemoryLoad, process_request->submitInfo);
	user_event.offset = load_order->offset;
	user_event.length = load_order->size;

	EventHub::Guard hub_guard(&process_request->eventHub->lock);
	process_request->eventHub->raiseEvent(hub_guard, frigg::move(user_event));
	hub_guard.unlock();
}

bool Memory::checkLock(LockRequest *lock_request) {
	assert(lock_request->size > 0);
	assert(lock_request->offset % kPageSize == 0);
	assert(lock_request->size % kPageSize == 0);
	
	for(uintptr_t page = 0; page < lock_request->size; page += kPageSize) {
		uintptr_t page_index = (lock_request->offset + page) / kPageSize;
		if(loadState[page_index] != kStateLoaded)
			return false;
	}

	return true;
}

void Memory::performLock(LockRequest *lock_request) {
	UserEvent user_event(UserEvent::kTypeMemoryLock, lock_request->submitInfo);

	EventHub::Guard hub_guard(&lock_request->eventHub->lock);
	lock_request->eventHub->raiseEvent(hub_guard, frigg::move(user_event));
	hub_guard.unlock();
}

// --------------------------------------------------------
// Memory::ProcessRequest
// --------------------------------------------------------

Memory::ProcessRequest::ProcessRequest(frigg::SharedPtr<EventHub> event_hub,
		SubmitInfo submit_info)
: eventHub(frigg::move(event_hub)), submitInfo(submit_info) { }

// --------------------------------------------------------
// Memory::LoadOrder
// --------------------------------------------------------

Memory::LoadOrder::LoadOrder(uintptr_t offset, size_t size)
: offset(offset), size(size) { }

// --------------------------------------------------------
// Memory::LockRequest
// --------------------------------------------------------

Memory::LockRequest::LockRequest(uintptr_t offset, size_t size,
		frigg::SharedPtr<EventHub> event_hub, SubmitInfo submit_info)
: offset(offset), size(size), eventHub(frigg::move(event_hub)), submitInfo(submit_info) { }

// --------------------------------------------------------
// Mapping
// --------------------------------------------------------

Mapping::Mapping(Type type, VirtualAddr base_address, size_t length)
: baseAddress(base_address), length(length), type(type),
		lowerPtr(nullptr), higherPtr(nullptr),
		leftPtr(nullptr), rightPtr(nullptr),
		parentPtr(nullptr), color(kColorNone), largestHole(0),
		memoryOffset(0), flags(0), writePermission(false), executePermission(false) {
	if(type == kTypeHole)
		largestHole = length;
}

Mapping::~Mapping() {
	frigg::destruct(*kernelAlloc, leftPtr);
	frigg::destruct(*kernelAlloc, rightPtr);
}

// --------------------------------------------------------
// AddressSpace
// --------------------------------------------------------

AddressSpace::AddressSpace(PageSpace page_space)
: p_root(nullptr), p_pageSpace(page_space) { }

AddressSpace::~AddressSpace() {
	frigg::destruct(*kernelAlloc, p_root);
}

void AddressSpace::setupDefaultMappings() {
	auto mapping = frigg::construct<Mapping>(*kernelAlloc, Mapping::kTypeHole,
			0x100000, 0x7ffffff00000);
	addressTreeInsert(mapping);
}

void AddressSpace::map(Guard &guard,
		KernelUnsafePtr<Memory> memory, VirtualAddr address,
		size_t offset, size_t length, uint32_t flags, VirtualAddr *actual_address) {
	assert(guard.protects(&lock));
	assert((length % kPageSize) == 0);

	Mapping *mapping;
	if((flags & kMapFixed) != 0) {
		assert((address % kPageSize) == 0);
		mapping = allocateAt(address, length);
	}else{
		mapping = allocate(length, flags);
	}
	assert(mapping != nullptr);

	mapping->type = Mapping::kTypeMemory;
	mapping->memoryRegion = KernelSharedPtr<Memory>(memory);
	mapping->memoryOffset = offset;

	uint32_t page_flags = 0;

	constexpr uint32_t mask = kMapReadOnly | kMapReadExecute | kMapReadWrite;
	if((flags & mask) == kMapReadWrite) {
		page_flags |= PageSpace::kAccessWrite;
		mapping->writePermission = true;
	}else if((flags & mask) == kMapReadExecute) {
		page_flags |= PageSpace::kAccessExecute;
		mapping->executePermission = true;
	}else{
		assert((flags & mask) == kMapReadOnly);
	}

	if(flags & kMapShareOnFork)
		mapping->flags |= Mapping::kFlagShareOnFork;

	if(memory->getType() == Memory::kTypeAllocated
			|| memory->getType() == Memory::kTypePhysical
			|| memory->getType() == Memory::kTypeOnDemand) {
		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock, frigg::dontLock);
		for(size_t page = 0; page < length; page += kPageSize) {
			VirtualAddr vaddr = mapping->baseAddress + page;
			assert(!p_pageSpace.isMapped(vaddr));

			PhysicalAddr physical = memory->getPageAt(offset + page);
			if(physical == PhysicalAddr(-1))
				continue;
			p_pageSpace.mapSingle4k(physical_guard, vaddr, physical, true, page_flags);
		}
		if(physical_guard.isLocked())
			physical_guard.unlock();
	}else if(memory->getType() == Memory::kTypeBacked
			&& (flags & kMapBacking)) {
		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock, frigg::dontLock);
		for(size_t page = 0; page < length; page += kPageSize) {
			VirtualAddr vaddr = mapping->baseAddress + page;
			assert(!p_pageSpace.isMapped(vaddr));

			PhysicalAddr physical = memory->getPageAt(offset + page);
			assert(physical != PhysicalAddr(-1));
			p_pageSpace.mapSingle4k(physical_guard, vaddr, physical, true, page_flags);
		}
		if(physical_guard.isLocked())
			physical_guard.unlock();
	}else if(memory->getType() == Memory::kTypeBacked) {
		// map non-loaded pages inside the page fault handler
		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock, frigg::dontLock);
		for(size_t page = 0; page < length; page += kPageSize) {
			VirtualAddr vaddr = mapping->baseAddress + page;
			assert(!p_pageSpace.isMapped(vaddr));
			
			if(memory->loadState[(offset + page) / kPageSize] != Memory::kStateLoaded)
				continue;

			PhysicalAddr physical = memory->getPageAt(offset + page);
			assert(physical != PhysicalAddr(-1));
			p_pageSpace.mapSingle4k(physical_guard, vaddr, physical, true, page_flags);
		}
	}else{
		frigg::panicLogger.log() << "Illegal memory type" << frigg::EndLog();
	}

	*actual_address = mapping->baseAddress;
}

void AddressSpace::unmap(Guard &guard, VirtualAddr address, size_t length) {
	Mapping *mapping = getMapping(address);
	assert(mapping != nullptr);
	assert(mapping->type == Mapping::kTypeMemory);

	// TODO: allow shrink of mapping
	assert(mapping->baseAddress == address);
	assert(mapping->length == length);
	
	for(size_t i = 0; i < mapping->length / kPageSize; i++) {
		VirtualAddr vaddr = mapping->baseAddress + i * kPageSize;
		if(p_pageSpace.isMapped(vaddr))
			p_pageSpace.unmapSingle4k(vaddr);
	}

	mapping->memoryRegion.reset();

	Mapping *lower_ptr = mapping->lowerPtr;
	Mapping *higher_ptr = mapping->higherPtr;
	
	if(lower_ptr && higher_ptr
			&& lower_ptr->type == Mapping::kTypeHole
			&& higher_ptr->type == Mapping::kTypeHole) {
		// grow the lower region and remove both the mapping and the higher region
		size_t mapping_length = mapping->length;
		size_t higher_length = higher_ptr->length;

		addressTreeRemove(mapping);
		addressTreeRemove(higher_ptr);
		frigg::destruct(*kernelAlloc, mapping);
		frigg::destruct(*kernelAlloc, higher_ptr);

		lower_ptr->length += mapping_length + higher_length;
		updateLargestHoleUpwards(lower_ptr);
	}else if(lower_ptr && lower_ptr->type == Mapping::kTypeHole) {
		// grow the lower region and remove the mapping
		size_t mapping_length = mapping->length;

		addressTreeRemove(mapping);
		frigg::destruct(*kernelAlloc, mapping);
		
		lower_ptr->length += mapping_length;
		updateLargestHoleUpwards(lower_ptr);
	}else if(higher_ptr && higher_ptr->type == Mapping::kTypeHole) {
		// grow the higher region and remove the mapping
		size_t mapping_length = mapping->length;

		addressTreeRemove(mapping);
		frigg::destruct(*kernelAlloc, mapping);
		
		higher_ptr->baseAddress -= mapping_length;
		higher_ptr->length += mapping_length;
		updateLargestHoleUpwards(higher_ptr);
	}else{
		// turn the mapping into a hole
		mapping->type = Mapping::kTypeHole;
		updateLargestHoleUpwards(mapping);
	}
}

bool AddressSpace::handleFault(Guard &guard, VirtualAddr address, uint32_t flags) {
	Mapping *mapping = getMapping(address);
	if(!mapping)
		return false;
	if(mapping->type != Mapping::kTypeMemory)
		return false;
	
	VirtualAddr page_vaddr = address - (address % kPageSize);
	VirtualAddr page_offset = page_vaddr - mapping->baseAddress;

	uint32_t page_flags = 0;
	if(mapping->writePermission)
		page_flags |= PageSpace::kAccessWrite;
	if(mapping->executePermission);
		page_flags |= PageSpace::kAccessExecute;
	
	KernelUnsafePtr<Memory> memory = mapping->memoryRegion;
	if(memory->getType() == Memory::kTypeOnDemand) {
		assert(memory->getPageAt(mapping->memoryOffset + page_offset) == PhysicalAddr(-1));

		// allocate a new page
		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
		PhysicalAddr physical = physicalAllocator->allocate(physical_guard, kPageSize);
		memset(physicalToVirtual(physical), 0, kPageSize);

		memory->setPageAt(mapping->memoryOffset + page_offset, physical);
		
		// map the new page into the address space
		p_pageSpace.mapSingle4k(physical_guard, page_vaddr, physical, true, page_flags);
		physical_guard.unlock();

		return true;
	}else if(memory->getType() == Memory::kTypeBacked) {
		// submit a load request for the page
		size_t page_index = (mapping->memoryOffset + page_offset) / kPageSize;
		if(memory->loadState[page_index] == Memory::kStateMissing)
			memory->loadMemory(mapping->memoryOffset + page_offset, kPageSize);

		// wait until the page is loaded
		while(memory->loadState[page_index] == Memory::kStateLoading) {
			assert(!intsAreEnabled());

			void *restore_state = __builtin_alloca(getStateSize());
			if(forkState(restore_state)) {
				KernelUnsafePtr<Thread> this_thread = getCurrentThread();
				memory->waitQueue.addBack(frigg::SharedPtr<Thread>(this_thread));
				
				resetCurrentThread(restore_state);
				ScheduleGuard schedule_guard(scheduleLock.get());
				doSchedule(frigg::move(schedule_guard));
				// note: doSchedule() takes care of the schedule_guard lock
			}
		}
	
		// map the page into the address space
		assert(memory->loadState[page_index] == Memory::kStateLoaded);

		PhysicalAddr physical = memory->getPageAt(mapping->memoryOffset + page_offset);
		assert(physical != PhysicalAddr(-1));

		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock, frigg::dontLock);
		p_pageSpace.mapSingle4k(physical_guard, page_vaddr, physical, true, page_flags);
		if(physical_guard.isLocked())
			physical_guard.unlock();
		
		return true;
	}else if(memory->getType() == Memory::kTypeCopyOnWrite) {
		assert(memory->getPageAt(mapping->memoryOffset + page_offset) == PhysicalAddr(-1));

		// allocate a new page and copy content from the master page
		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
		PhysicalAddr physical = physicalAllocator->allocate(physical_guard, kPageSize);
		physical_guard.unlock();
		
		PhysicalAddr origin = memory->master->resolveOriginalAt(mapping->memoryOffset
				+ page_offset);
		assert(origin != PhysicalAddr(-1)); // TODO: implement copy-on-write of on-demand pages
		memcpy(physicalToVirtual(physical), physicalToVirtual(origin), kPageSize);
		memory->setPageAt(mapping->memoryOffset + page_offset, physical);

		// map the new page into the address space
		if(p_pageSpace.isMapped(page_vaddr))
			p_pageSpace.unmapSingle4k(page_vaddr);
		p_pageSpace.mapSingle4k(physical_guard, page_vaddr, physical, true, page_flags);
		if(physical_guard.isLocked())
			physical_guard.unlock();

		return true;
	}

	return false;
}

KernelSharedPtr<AddressSpace> AddressSpace::fork(Guard &guard) {
	assert(guard.protects(&lock));

	auto forked = frigg::makeShared<AddressSpace>(*kernelAlloc,
			kernelSpace->cloneFromKernelSpace());

	cloneRecursive(p_root, forked.get());

	return frigg::move(forked);
}
	
PhysicalAddr AddressSpace::getPhysical(Guard &guard, VirtualAddr address) {
	assert(guard.protects(&lock));
	assert((address % kPageSize) == 0);

	Mapping *mapping = getMapping(address);
	assert(mapping);
	assert(mapping->type == Mapping::kTypeMemory);
	assert(mapping->memoryRegion->getType() == Memory::kTypeAllocated
			|| mapping->memoryRegion->getType() == Memory::kTypeBacked);

	auto page = address - mapping->baseAddress;
	PhysicalAddr physical = mapping->memoryRegion->getPageAt(mapping->memoryOffset + page);
	assert(physical != PhysicalAddr(-1));
	return physical;
}

void AddressSpace::activate() {
	p_pageSpace.activate();
}

Mapping *AddressSpace::getMapping(VirtualAddr address) {
	Mapping *current = p_root;
	
	while(current != nullptr) {
		if(address < current->baseAddress) {
			current = current->leftPtr;
		}else if(address >= current->baseAddress + current->length) {
			current = current->rightPtr;
		}else{
			assert(address >= current->baseAddress
					&& address < current->baseAddress + current->length);
			return current;
		}
	}

	return nullptr;
}

Mapping *AddressSpace::allocate(size_t length, MapFlags flags) {
	assert(length > 0);
	assert((length % kPageSize) == 0);
//	infoLogger->log() << "Allocate virtual memory area"
//			<< ", size: 0x" << frigg::logHex(length) << frigg::EndLog();

	if(p_root->largestHole < length)
		return nullptr;
	
	return allocateDfs(p_root, length, flags);
}

Mapping *AddressSpace::allocateDfs(Mapping *mapping, size_t length,
		MapFlags flags) {
	if((flags & kMapPreferBottom) != 0) {
		// try to allocate memory at the bottom of the range
		if(mapping->type == Mapping::kTypeHole && mapping->length >= length)
			return splitHole(mapping, 0, length);
		
		if(mapping->leftPtr && mapping->leftPtr->largestHole >= length)
			return allocateDfs(mapping->leftPtr, length, flags);
		
		assert(mapping->rightPtr && mapping->rightPtr->largestHole >= length);
		return allocateDfs(mapping->rightPtr, length, flags);
	}else{
		// try to allocate memory at the top of the range
		assert((flags & kMapPreferTop) != 0);
		if(mapping->type == Mapping::kTypeHole && mapping->length >= length)
			return splitHole(mapping, mapping->length - length, length);

		if(mapping->rightPtr && mapping->rightPtr->largestHole >= length)
			return allocateDfs(mapping->rightPtr, length, flags);
		
		assert(mapping->leftPtr && mapping->leftPtr->largestHole >= length);
		return allocateDfs(mapping->leftPtr, length, flags);
	}
}

Mapping *AddressSpace::allocateAt(VirtualAddr address, size_t length) {
	assert((address % kPageSize) == 0);
	assert((length % kPageSize) == 0);

	Mapping *hole = getMapping(address);
	assert(hole != nullptr);
	assert(hole->type == Mapping::kTypeHole);
	
	return splitHole(hole, address - hole->baseAddress, length);
}

void AddressSpace::cloneRecursive(Mapping *mapping, AddressSpace *dest_space) {
	Mapping *dest_mapping = frigg::construct<Mapping>(*kernelAlloc, mapping->type,
			mapping->baseAddress, mapping->length);

	if(mapping->type == Mapping::kTypeHole) {
		// holes do not require additional handling
	}else if(mapping->type == Mapping::kTypeMemory
			&& (mapping->flags & Mapping::kFlagShareOnFork)) {
		KernelUnsafePtr<Memory> memory = mapping->memoryRegion;
		assert(memory->getType() == Memory::kTypeAllocated
				|| memory->getType() == Memory::kTypePhysical
				|| memory->getType() == Memory::kTypeBacked);

		uint32_t page_flags = 0;
		if(mapping->writePermission)
			page_flags |= PageSpace::kAccessWrite;
		if(mapping->executePermission);
			page_flags |= PageSpace::kAccessExecute;

		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock, frigg::dontLock);
		for(size_t page = 0; page < dest_mapping->length; page += kPageSize) {
			PhysicalAddr physical = memory->getPageAt(mapping->memoryOffset + page);
			if(physical == PhysicalAddr(-1))
				continue;
			VirtualAddr vaddr = dest_mapping->baseAddress + page;
			dest_space->p_pageSpace.mapSingle4k(physical_guard, vaddr, physical, true, page_flags);
		}
		if(physical_guard.isLocked())
			physical_guard.unlock();
		
		dest_mapping->memoryRegion = KernelSharedPtr<Memory>(memory);
		dest_mapping->memoryOffset = mapping->memoryOffset;
		dest_mapping->writePermission = mapping->writePermission;
		dest_mapping->executePermission = mapping->executePermission;
	}else if(mapping->type == Mapping::kTypeMemory) {
		KernelUnsafePtr<Memory> memory = mapping->memoryRegion;
		assert(memory->getType() == Memory::kTypeAllocated
				|| memory->getType() == Memory::kTypeOnDemand
				|| memory->getType() == Memory::kTypeCopyOnWrite);

		// don't set the write flag to enable copy-on-write
		uint32_t page_flags = 0;
		if(mapping->executePermission);
			page_flags |= PageSpace::kAccessExecute;
		
		// create a copy-on-write region for the original space
		auto src_memory = frigg::makeShared<Memory>(*kernelAlloc, Memory::kTypeCopyOnWrite);
		src_memory->resize(memory->numPages());
		src_memory->master = KernelSharedPtr<Memory>(memory);
		mapping->memoryRegion = frigg::move(src_memory);
		
		PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
		for(size_t page = 0; page < mapping->length; page += kPageSize) {
			PhysicalAddr physical = memory->resolveOriginalAt(mapping->memoryOffset + page);
			if(physical == PhysicalAddr(-1))
				continue;
			VirtualAddr vaddr = mapping->baseAddress + page;
			p_pageSpace.unmapSingle4k(vaddr);
			p_pageSpace.mapSingle4k(physical_guard, vaddr, physical, true, page_flags);
		}
		// we need to release the lock before calling makeShared()
		if(physical_guard.isLocked())
			physical_guard.unlock();
		
		// create a copy-on-write region for the forked space
		auto dest_memory = frigg::makeShared<Memory>(*kernelAlloc, Memory::kTypeCopyOnWrite);
		dest_memory->resize(memory->numPages());
		dest_memory->master = KernelSharedPtr<Memory>(memory);
		dest_mapping->memoryRegion = frigg::move(dest_memory);
		
		for(size_t page = 0; page < mapping->length; page += kPageSize) {
			PhysicalAddr physical = memory->resolveOriginalAt(mapping->memoryOffset + page);
			if(physical == PhysicalAddr(-1))
				continue;
			VirtualAddr vaddr = mapping->baseAddress + page;
			dest_space->p_pageSpace.mapSingle4k(physical_guard, vaddr, physical, true, page_flags);
		}
		if(physical_guard.isLocked())
			physical_guard.unlock();
		
		dest_mapping->writePermission = mapping->writePermission;
		dest_mapping->executePermission = mapping->executePermission;
		dest_mapping->memoryOffset = mapping->memoryOffset;
	}else{
		assert(!"Illegal mapping type");
	}

	dest_space->addressTreeInsert(dest_mapping);

	if(mapping->leftPtr)
		cloneRecursive(mapping->leftPtr, dest_space);
	if(mapping->rightPtr)
		cloneRecursive(mapping->rightPtr, dest_space);
}

Mapping *AddressSpace::splitHole(Mapping *mapping,
		VirtualAddr split_offset, size_t split_length) {
	assert(split_length > 0);
	assert(mapping->type == Mapping::kTypeHole);
	assert(split_offset + split_length <= mapping->length);
	
	VirtualAddr hole_address = mapping->baseAddress;
	size_t hole_length = mapping->length;
	
	if(split_offset == 0) {
		// the split mapping starts at the beginning of the hole
		// we have to delete the hole mapping
		addressTreeRemove(mapping);
		frigg::destruct(*kernelAlloc, mapping);
	}else{
		// the split mapping starts in the middle of the hole
		mapping->length = split_offset;
		updateLargestHoleUpwards(mapping);
	}

	auto split = frigg::construct<Mapping>(*kernelAlloc, Mapping::kTypeNone,
			hole_address + split_offset, split_length);
	addressTreeInsert(split);

	if(hole_length > split_offset + split_length) {
		// the split mapping does not go on until the end of the hole
		// we have to create another mapping for the rest of the hole
		auto following = frigg::construct<Mapping>(*kernelAlloc, Mapping::kTypeHole,
				hole_address + (split_offset + split_length),
				hole_length - (split_offset + split_length));
		addressTreeInsert(following);
	}else{
		assert(hole_length == split_offset + split_length);
	}

	return split;
}

void AddressSpace::rotateLeft(Mapping *n) {
	Mapping *u = n->parentPtr;
	assert(u != nullptr && u->rightPtr == n);
	Mapping *v = n->leftPtr;
	Mapping *w = u->parentPtr;

	if(v != nullptr)
		v->parentPtr = u;
	u->rightPtr = v;
	u->parentPtr = n;
	n->leftPtr = u;
	n->parentPtr = w;

	if(w == nullptr) {
		p_root = n;
	}else if(w->leftPtr == u) {
		w->leftPtr = n;
	}else{
		assert(w->rightPtr == u);
		w->rightPtr = n;
	}

	updateLargestHoleAt(u);
	updateLargestHoleAt(n);
}

void AddressSpace::rotateRight(Mapping *n) {
	Mapping *u = n->parentPtr;
	assert(u != nullptr && u->leftPtr == n);
	Mapping *v = n->rightPtr;
	Mapping *w = u->parentPtr;
	
	if(v != nullptr)
		v->parentPtr = u;
	u->leftPtr = v;
	u->parentPtr = n;
	n->rightPtr = u;
	n->parentPtr = w;

	if(w == nullptr) {
		p_root = n;
	}else if(w->leftPtr == u) {
		w->leftPtr = n;
	}else{
		assert(w->rightPtr == u);
		w->rightPtr = n;
	}

	updateLargestHoleAt(u);
	updateLargestHoleAt(n);
}

bool AddressSpace::isRed(Mapping *mapping) {
	if(mapping == nullptr)
		return false;
	return mapping->color == Mapping::kColorRed;
}
bool AddressSpace::isBlack(Mapping *mapping) {
	if(mapping == nullptr)
		return true;
	return mapping->color == Mapping::kColorBlack;
}

void AddressSpace::addressTreeInsert(Mapping *mapping) {
	assert(checkInvariant());

	if(!p_root) {
		p_root = mapping;

		fixAfterInsert(mapping);
		assert(checkInvariant());
		return;
	}

	Mapping *current = p_root;
	while(true) {
		if(mapping->baseAddress < current->baseAddress) {
			assert(mapping->baseAddress + mapping->length <= current->baseAddress);
			if(current->leftPtr == nullptr) {
				current->leftPtr = mapping;
				mapping->parentPtr = current;

				// "current" is the successor of "mapping"
				Mapping *predecessor = current->lowerPtr;
				if(predecessor)
					predecessor->higherPtr = mapping;
				mapping->lowerPtr = predecessor;
				mapping->higherPtr = current;
				current->lowerPtr = mapping;

				updateLargestHoleUpwards(current);

				fixAfterInsert(mapping);
				assert(checkInvariant());
				return;
			}else{
				current = current->leftPtr;
			}
		}else{
			assert(mapping->baseAddress >= current->baseAddress + current->length);
			if(current->rightPtr == nullptr) {
				current->rightPtr = mapping;
				mapping->parentPtr = current;

				// "current" is the predecessor of "mapping"
				Mapping *successor = current->higherPtr;
				current->higherPtr = mapping;
				mapping->lowerPtr = current;
				mapping->higherPtr = successor;
				if(successor)
					successor->lowerPtr = mapping;
				
				updateLargestHoleUpwards(current);
				
				fixAfterInsert(mapping);
				assert(checkInvariant());
				return;
			}else{
				current = current->rightPtr;
			}
		}
	}
}

// Situation:
// |     (p)     |
// |    /   \    |
// |  (s)   (n)  |
// Precondition: The red-black property is only violated in the following sense:
//     Paths from (p) over (n) to a leaf contain one black node more
//     than paths from (p) over (s) to a leaf
// Postcondition: The whole tree is a red-black tree
void AddressSpace::fixAfterInsert(Mapping *n) {
	Mapping *parent = n->parentPtr;
	if(parent == nullptr) {
		n->color = Mapping::kColorBlack;
		return;
	}
	
	n->color = Mapping::kColorRed;

	if(parent->color == Mapping::kColorBlack)
		return;
	
	// the rb invariants guarantee that a grandparent exists
	Mapping *grand = parent->parentPtr;
	assert(grand && grand->color == Mapping::kColorBlack);
	
	// handle the red uncle case
	if(grand->leftPtr == parent && isRed(grand->rightPtr)) {
		grand->color = Mapping::kColorRed;
		parent->color = Mapping::kColorBlack;
		grand->rightPtr->color = Mapping::kColorBlack;

		fixAfterInsert(grand);
		return;
	}else if(grand->rightPtr == parent && isRed(grand->leftPtr)) {
		grand->color = Mapping::kColorRed;
		parent->color = Mapping::kColorBlack;
		grand->leftPtr->color = Mapping::kColorBlack;

		fixAfterInsert(grand);
		return;
	}
	
	if(parent == grand->leftPtr) {
		if(n == parent->rightPtr) {
			rotateLeft(n);
			rotateRight(n);
			n->color = Mapping::kColorBlack;
		}else{
			rotateRight(parent);
			parent->color = Mapping::kColorBlack;
		}
		grand->color = Mapping::kColorRed;
	}else{
		assert(parent == grand->rightPtr);
		if(n == parent->leftPtr) {
			rotateRight(n);
			rotateLeft(n);
			n->color = Mapping::kColorBlack;
		}else{
			rotateLeft(parent);
			parent->color = Mapping::kColorBlack;
		}
		grand->color = Mapping::kColorRed;
	}
}

void AddressSpace::addressTreeRemove(Mapping *mapping) {
	assert(checkInvariant());

	Mapping *left_ptr = mapping->leftPtr;
	Mapping *right_ptr = mapping->rightPtr;

	if(!left_ptr) {
		removeHalfLeaf(mapping, right_ptr);
	}else if(!right_ptr) {
		removeHalfLeaf(mapping, left_ptr);
	}else{
		// replace the mapping by its predecessor
		Mapping *predecessor = mapping->lowerPtr;
		removeHalfLeaf(predecessor, predecessor->leftPtr);
		replaceNode(mapping, predecessor);
	}
	
	assert(checkInvariant());
}

void AddressSpace::replaceNode(Mapping *node, Mapping *replacement) {
	Mapping *parent = node->parentPtr;
	Mapping *left = node->leftPtr;
	Mapping *right = node->rightPtr;

	// fix the red-black tree
	if(parent == nullptr) {
		p_root = replacement;
	}else if(node == parent->leftPtr) {
		parent->leftPtr = replacement;
	}else{
		assert(node == parent->rightPtr);
		parent->rightPtr = replacement;
	}
	replacement->parentPtr = parent;
	replacement->color = node->color;

	replacement->leftPtr = left;
	if(left)
		left->parentPtr = replacement;
	
	replacement->rightPtr = right;
	if(right)
		right->parentPtr = replacement;
	
	// fix the linked list
	if(node->lowerPtr)
		node->lowerPtr->higherPtr = replacement;
	replacement->lowerPtr = node->lowerPtr;
	replacement->higherPtr = node->higherPtr;
	if(node->higherPtr)
		node->higherPtr->lowerPtr = replacement;
	
	node->leftPtr = nullptr;
	node->rightPtr = nullptr;
	node->parentPtr = nullptr;
	node->lowerPtr = nullptr;
	node->higherPtr = nullptr;
	
	updateLargestHoleAt(replacement);
	updateLargestHoleUpwards(parent);
}

void AddressSpace::removeHalfLeaf(Mapping *mapping, Mapping *child) {
	Mapping *predecessor = mapping->lowerPtr;
	Mapping *successor = mapping->higherPtr;
	if(predecessor)
		predecessor->higherPtr = successor;
	if(successor)
		successor->lowerPtr = predecessor;

	if(mapping->color == Mapping::kColorBlack) {
		if(isRed(child)) {
			child->color = Mapping::kColorBlack;
		}else{
			// decrement the number of black nodes all paths through "mapping"
			// before removing the child. this makes sure we're correct even when
			// "child" is null
			fixAfterRemove(mapping);
		}
	}
	
	assert((!mapping->leftPtr && mapping->rightPtr == child)
			|| (mapping->leftPtr == child && !mapping->rightPtr));
		
	Mapping *parent = mapping->parentPtr;
	if(!parent) {
		p_root = child;
	}else if(parent->leftPtr == mapping) {
		parent->leftPtr = child;
	}else{
		assert(parent->rightPtr == mapping);
		parent->rightPtr = child;
	}
	if(child)
		child->parentPtr = parent;
	
	mapping->leftPtr = nullptr;
	mapping->rightPtr = nullptr;
	mapping->parentPtr = nullptr;
	mapping->lowerPtr = nullptr;
	mapping->higherPtr = nullptr;
	
	if(parent)
		updateLargestHoleUpwards(parent);
}

// Situation:
// |     (p)     |
// |    /   \    |
// |  (s)   (n)  |
// Precondition: The red-black property is only violated in the following sense:
//     Paths from (p) over (n) to a leaf contain one black node less
//     than paths from (p) over (s) to a leaf
// Postcondition: The whole tree is a red-black tree
void AddressSpace::fixAfterRemove(Mapping *n) {
	assert(n->color == Mapping::kColorBlack);
	
	Mapping *parent = n->parentPtr;
	if(parent == nullptr)
		return;
	
	// rotate so that our node has a black sibling
	Mapping *s; // this will always be the sibling of our node
	if(parent->leftPtr == n) {
		assert(parent->rightPtr);
		if(parent->rightPtr->color == Mapping::kColorRed) {
			Mapping *x = parent->rightPtr;
			rotateLeft(parent->rightPtr);
			assert(n == parent->leftPtr);
			
			parent->color = Mapping::kColorRed;
			x->color = Mapping::kColorBlack;
		}
		
		s = parent->rightPtr;
	}else{
		assert(parent->rightPtr == n);
		assert(parent->leftPtr);
		if(parent->leftPtr->color == Mapping::kColorRed) {
			Mapping *x = parent->leftPtr;
			rotateRight(x);
			assert(n == parent->rightPtr);
			
			parent->color = Mapping::kColorRed;
			x->color = Mapping::kColorBlack;
		}
		
		s = parent->leftPtr;
	}
	
	if(isBlack(s->leftPtr) && isBlack(s->rightPtr)) {
		if(parent->color == Mapping::kColorBlack) {
			s->color = Mapping::kColorRed;
			fixAfterRemove(parent);
			return;
		}else{
			parent->color = Mapping::kColorBlack;
			s->color = Mapping::kColorRed;
			return;
		}
	}
	
	// now at least one of s children is red
	Mapping::Color parent_color = parent->color;
	if(parent->leftPtr == n) {
		// rotate so that s->rightPtr is red
		if(isRed(s->leftPtr) && isBlack(s->rightPtr)) {
			Mapping *child = s->leftPtr;
			rotateRight(child);

			s->color = Mapping::kColorRed;
			child->color = Mapping::kColorBlack;

			s = child;
		}
		assert(isRed(s->rightPtr));

		rotateLeft(s);
		parent->color = Mapping::kColorBlack;
		s->color = parent_color;
		s->rightPtr->color = Mapping::kColorBlack;
	}else{
		assert(parent->rightPtr == n);

		// rotate so that s->leftPtr is red
		if(isRed(s->rightPtr) && isBlack(s->leftPtr)) {
			Mapping *child = s->rightPtr;
			rotateLeft(child);

			s->color = Mapping::kColorRed;
			child->color = Mapping::kColorBlack;

			s = child;
		}
		assert(isRed(s->leftPtr));

		rotateRight(s);
		parent->color = Mapping::kColorBlack;
		s->color = parent_color;
		s->leftPtr->color = Mapping::kColorBlack;
	}
}

bool AddressSpace::checkInvariant() {
	if(!p_root)
		return true;

	int black_depth;
	Mapping *minimal, *maximal;
	return checkInvariant(p_root, black_depth, minimal, maximal);
}

bool AddressSpace::checkInvariant(Mapping *mapping, int &black_depth,
		Mapping *&minimal, Mapping *&maximal) {
	// check largest hole invariant
	size_t hole = 0;
	if(mapping->type == Mapping::kTypeHole)
		hole = mapping->length;
	if(mapping->leftPtr && mapping->leftPtr->largestHole > hole)
		hole = mapping->leftPtr->largestHole;
	if(mapping->rightPtr && mapping->rightPtr->largestHole > hole)
		hole = mapping->rightPtr->largestHole;
	
	if(mapping->largestHole != hole) {
		infoLogger->log() << "largestHole violation" << frigg::EndLog();
		return false;
	}

	// check alternating colors invariant
	if(mapping->color == Mapping::kColorRed)
		if(!isBlack(mapping->leftPtr) || !isBlack(mapping->rightPtr)) {
			infoLogger->log() << "Alternating colors violation" << frigg::EndLog();
			return false;
		}
	
	// check recursive invariants
	int left_black_depth = 0;
	int right_black_depth = 0;
	
	if(mapping->leftPtr) {
		Mapping *predecessor;
		if(!checkInvariant(mapping->leftPtr, left_black_depth, minimal, predecessor))
			return false;

		// check search tree invariant
		if(mapping->baseAddress < predecessor->baseAddress + predecessor->length) {
			infoLogger->log() << "Search tree (left) violation" << frigg::EndLog();
			return false;
		}
		
		// check predecessor invariant
		if(predecessor->higherPtr != mapping) {
			infoLogger->log() << "Linked list (predecessor, forward) violation" << frigg::EndLog();
			return false;
		}else if(mapping->lowerPtr != predecessor) {
			infoLogger->log() << "Linked list (predecessor, backward) violation" << frigg::EndLog();
			return false;
		}
	}else{
		minimal = mapping;
	}

	if(mapping->rightPtr) {
		Mapping *successor;
		if(!checkInvariant(mapping->rightPtr, right_black_depth, successor, maximal))
			return false;
		
		// check search tree invariant
		if(mapping->baseAddress + mapping->length > successor->baseAddress) {
			infoLogger->log() << "Search tree (right) violation" << frigg::EndLog();
			return false;
		}

		// check successor invariant
		if(mapping->higherPtr != successor) {
			infoLogger->log() << "Linked list (successor, forward) violation" << frigg::EndLog();
			return false;
		}else if(successor->lowerPtr != mapping) {
			infoLogger->log() << "Linked list (successor, backward) violation" << frigg::EndLog();
			return false;
		}
	}else{
		maximal = mapping;
	}
	
	// check black-depth invariant
	if(left_black_depth != right_black_depth) {
		infoLogger->log() << "Black-depth violation" << frigg::EndLog();
		return false;
	}

	black_depth = left_black_depth;
	if(mapping->color == Mapping::kColorBlack)
		black_depth++;
	
	return true;
}

bool AddressSpace::updateLargestHoleAt(Mapping *mapping) {
	size_t hole = 0;
	if(mapping->type == Mapping::kTypeHole)
		hole = mapping->length;
	if(mapping->leftPtr && mapping->leftPtr->largestHole > hole)
		hole = mapping->leftPtr->largestHole;
	if(mapping->rightPtr && mapping->rightPtr->largestHole > hole)
		hole = mapping->rightPtr->largestHole;
	
	if(mapping->largestHole != hole) {
		mapping->largestHole = hole;
		return true;
	}
	return false;
}
void AddressSpace::updateLargestHoleUpwards(Mapping *mapping) {
	if(!updateLargestHoleAt(mapping))
		return;
	
	if(mapping->parentPtr != nullptr)
		updateLargestHoleUpwards(mapping->parentPtr);
}

} // namespace thor

