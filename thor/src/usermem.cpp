
#include "../../frigg/include/types.hpp"
#include "util/general.hpp"
#include "runtime.hpp"
#include "debug.hpp"
#include "util/vector.hpp"
#include "util/smart-ptr.hpp"
#include "memory/physical-alloc.hpp"
#include "memory/paging.hpp"
#include "memory/kernel-alloc.hpp"
#include "core.hpp"

namespace thor {

// --------------------------------------------------------
// Memory
// --------------------------------------------------------

Memory::Memory()
		: p_physicalPages(kernelAlloc.get()) { }

void Memory::resize(size_t length) {
	for(size_t l = 0; l < length; l += 0x1000) {
		uintptr_t page = memory::tableAllocator->allocate(1);
		p_physicalPages.push(page);
	}
}

uintptr_t Memory::getPage(int index) {
	return p_physicalPages[index];
}

// --------------------------------------------------------
// MemoryAccessDescriptor
// --------------------------------------------------------

MemoryAccessDescriptor::MemoryAccessDescriptor(SharedPtr<Memory> &&memory)
		: p_memory(util::move(memory)) { }

UnsafePtr<Memory> MemoryAccessDescriptor::getMemory() {
	return p_memory->unsafe<Memory>();
}

// --------------------------------------------------------
// Mapping
// --------------------------------------------------------

Mapping::Mapping(Type type, VirtualAddr base_address, size_t length)
: baseAddress(base_address), length(length), type(type),
		lowerPtr(nullptr), higherPtr(nullptr),
		leftPtr(nullptr), rightPtr(nullptr), parentPtr(nullptr) { }

// --------------------------------------------------------
// AddressSpace
// --------------------------------------------------------

AddressSpace::AddressSpace(memory::PageSpace page_space)
: p_pageSpace(page_space) {
	p_root = new (kernelAlloc.get()) Mapping(Mapping::kTypeHole,
			0x100000, 0x7ffffff00000);
	p_root->largestHole = p_root->length;
}

void AddressSpace::mapSingle4k(void *address, uintptr_t physical) {
	p_pageSpace.mapSingle4k(address, physical);
}

Mapping *AddressSpace::getMapping(VirtualAddr address) {
	Mapping *current = p_root;

	while(current != nullptr) {
		if(address >= current->baseAddress
				&& address < current->baseAddress + current->length) {
			return current;
		}else if(address < current->baseAddress) {
			current = current->leftPtr;
		}else if(address >= current->baseAddress + current->length) {
			current = current->rightPtr;
		}else{
			debug::criticalLogger->log("Broken mapping tree");
			debug::panic();
		}
	}

	return nullptr;
}

Mapping *AddressSpace::allocate(size_t length) {
	if(p_root->largestHole < length)
		return nullptr;
	
	return allocateDfs(p_root, length);
}

Mapping *AddressSpace::allocateDfs(Mapping *mapping, size_t length) {
	if(mapping->type == Mapping::kTypeHole && mapping->length >= length)
		return splitHole(mapping, 0, length);
	
	if(mapping->leftPtr != nullptr
			&& mapping->leftPtr->largestHole >= length)
		return allocateDfs(mapping->leftPtr, length);
	
	if(mapping->rightPtr != nullptr
			&& mapping->rightPtr->largestHole >= length)
		return allocateDfs(mapping->rightPtr, length);
	
	debug::criticalLogger->log("Broken largestHole values");
	debug::panic();
}

Mapping *AddressSpace::allocateAt(VirtualAddr address, size_t length) {
	Mapping *hole = getMapping(address);
	if(hole == nullptr) {
		debug::criticalLogger->log("Address not in mapping");
		debug::panic();
	}

	if(hole->type != Mapping::kTypeHole) {
		debug::criticalLogger->log("Mapping is not a hole");
		debug::panic();
	}
	
	return splitHole(hole, address - hole->baseAddress, length);
}

Mapping *AddressSpace::splitHole(Mapping *mapping,
		VirtualAddr split_offset, size_t split_length) {
	if(mapping->type != Mapping::kTypeHole) {
		debug::criticalLogger->log("Mapping is not a hole");
		debug::panic();
	}
	if(split_offset + split_length > mapping->length) {
		debug::criticalLogger->log("Address out of mapping bounds");
		debug::panic();
	}
	if(split_length == 0) {
		debug::criticalLogger->log("split_length == 0");
		debug::panic();
	}

	Mapping *lower = mapping->lowerPtr;
	Mapping *higher = mapping->higherPtr;
	VirtualAddr hole_address = mapping->baseAddress;
	size_t hole_length = mapping->length;

	Mapping *split = new (kernelAlloc.get()) Mapping(Mapping::kTypeNone,
			hole_address + split_offset, split_length);
	
	if(split_offset == 0) {
		// the split mapping starts at the beginning of the hole
		// we have to delete the hole mapping
		split->lowerPtr = lower;
		if(lower != nullptr)
			lower->higherPtr = split;

		addressTreeRemove(mapping);

		//FIXME: delete (kernelAlloc.get()) mapping;
	}else{
		// the split mapping starts in the middle of the hole
		split->lowerPtr = mapping;
		mapping->higherPtr = split;

		mapping->length = split_offset;
	}

	addressTreeInsert(split);

	if(hole_length > split_offset + split_length) {
		// the split mapping does not go on until the end of the hole
		// we have to create another mapping for the rest of the hole
		Mapping *following = new (kernelAlloc.get()) Mapping(Mapping::kTypeHole,
				hole_address + split_offset + split_length,
				hole_length - split_offset - split_length);
		split->higherPtr = following;
		following->lowerPtr = split;
		following->higherPtr = higher;
		if(higher != nullptr)
			higher->lowerPtr = following;

		addressTreeInsert(following);
	}else if(hole_length == split_offset + split_length) {
		// the split mapping goes on until the end of the hole
		split->higherPtr = higher;
		if(higher != nullptr)
			higher->lowerPtr = split;
	}else{
		debug::criticalLogger->log("Address out of mapping bounds");
		debug::panic();
	}

	return split;
}

void AddressSpace::addressTreeInsert(Mapping *mapping) {
	if(p_root == nullptr) {
		p_root = mapping;
		return;
	}

	Mapping *current = p_root;

	while(true) {
		if(mapping->baseAddress < current->baseAddress) {
			if(current->leftPtr == nullptr) {
				current->leftPtr = mapping;
				mapping->parentPtr = current;

				updateLargestHole(mapping);

				return;
			}else{
				current = current->leftPtr;
			}
		}else if(mapping->baseAddress > current->baseAddress) {
			if(current->rightPtr == nullptr) {
				current->rightPtr = mapping;
				mapping->parentPtr = current;
				
				updateLargestHole(mapping);
				
				return;
			}else{
				current = current->rightPtr;
			}
		}else{
			debug::criticalLogger->log("Broken mapping tree");
			debug::panic();
		}
	}
}

void AddressSpace::addressTreeRemove(Mapping *mapping) {
	Mapping *parent = mapping->parentPtr;
	Mapping *left = mapping->leftPtr;
	Mapping *right = mapping->rightPtr;

	if(mapping->leftPtr == nullptr) {
		// replace the mapping by its right child
		if(parent == nullptr) {
			p_root = right;
		}else if(mapping == parent->leftPtr) {
			parent->leftPtr = right;
		}else if(mapping == parent->rightPtr) {
			parent->rightPtr = right;
		}else{
			debug::criticalLogger->log("Broken mapping tree");
			debug::panic();
		}
		if(right)
			right->parentPtr = parent;
	}else if(mapping->rightPtr == nullptr) {
		// replace the mapping by its left child
		if(parent == nullptr) {
			p_root = left;
		}else if(mapping == parent->leftPtr) {
			parent->leftPtr = left;
		}else if(mapping == parent->rightPtr) {
			parent->rightPtr = left;
		}else{
			debug::criticalLogger->log("Broken mapping tree");
			debug::panic();
		}
		if(left)
			left->parentPtr = parent;
	}else{
		// TODO: replace by mapping->lowerPtr
		Mapping *predecessor = mapping->leftPtr;
		while(predecessor->rightPtr != nullptr)
			predecessor = predecessor->rightPtr;

		// replace the predecessor by its left child
		Mapping *pre_parent = predecessor->parentPtr;
		Mapping *pre_left = predecessor->leftPtr;
		if(predecessor == pre_parent->leftPtr) {
			pre_parent->leftPtr = pre_left;
		}else if(predecessor == pre_parent->rightPtr) {
			pre_parent->rightPtr = pre_left;
		}else{
			debug::criticalLogger->log("Broken mapping tree");
			debug::panic();
		}
		if(pre_left)
			pre_left->parentPtr = pre_parent;
		
		updateLargestHole(pre_parent);

		// replace the mapping by its predecessor
		if(parent == nullptr) {
			p_root = predecessor;
		}else if(mapping == parent->leftPtr) {
			parent->leftPtr = predecessor;
		}else if(mapping == parent->rightPtr) {
			parent->rightPtr = predecessor;
		}else{
			debug::criticalLogger->log("Broken mapping tree");
			debug::panic();
		}
		predecessor->leftPtr = left;
		left->parentPtr = predecessor;
		predecessor->rightPtr = right;
		right->parentPtr = predecessor;
		predecessor->parentPtr = parent;
	}
	
	if(parent != nullptr)
		updateLargestHole(parent);
}

void AddressSpace::updateLargestHole(Mapping *mapping) {
	size_t hole = 0;
	if(mapping->type == Mapping::kTypeHole)
		hole = mapping->length;
	
	if(mapping->leftPtr != nullptr
			&& mapping->leftPtr->largestHole > hole)
		hole = mapping->leftPtr->largestHole;
	
	if(mapping->rightPtr != nullptr
			&& mapping->rightPtr->largestHole > hole)
		hole = mapping->rightPtr->largestHole;
	
	mapping->largestHole = hole;
	
	if(mapping->parentPtr != nullptr)
		updateLargestHole(mapping->parentPtr);
}

} // namespace thor

