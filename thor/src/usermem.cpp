
#include "kernel.hpp"

namespace traits = frigg::traits;
namespace debug = frigg::debug;
//FIXME: namespace memory = frigg::memory;

namespace thor {

// --------------------------------------------------------
// Memory
// --------------------------------------------------------

Memory::Memory()
: p_physicalPages(*kernelAlloc) { }

void Memory::resize(size_t length) {
	for(size_t l = 0; l < length; l += 0x1000) {
		PhysicalAddr page = physicalAllocator->allocate(1);
		p_physicalPages.push(page);
	}
}

void Memory::addPage(PhysicalAddr page) {
	p_physicalPages.push(page);
}

PhysicalAddr Memory::getPage(int index) {
	return p_physicalPages[index];
}

size_t Memory::getSize() {
	return p_physicalPages.size() * 0x1000;
}

// --------------------------------------------------------
// Mapping
// --------------------------------------------------------

Mapping::Mapping(Type type, VirtualAddr base_address, size_t length)
: baseAddress(base_address), length(length), type(type),
		lowerPtr(nullptr), higherPtr(nullptr),
		leftPtr(nullptr), rightPtr(nullptr),
		parentPtr(nullptr), color(kColorNone),
		memoryOffset(0) {
	if(type == kTypeHole)
		largestHole = length;
}

// --------------------------------------------------------
// AddressSpace
// --------------------------------------------------------

AddressSpace::AddressSpace(PageSpace page_space)
: p_pageSpace(page_space) {
	auto mapping = frigg::memory::construct<Mapping>(*kernelAlloc, Mapping::kTypeHole,
			0x100000, 0x7ffffff00000);
	addressTreeInsert(mapping);
}

void AddressSpace::map(UnsafePtr<Memory, KernelAlloc> memory, VirtualAddr address,
		size_t length, uint32_t flags, VirtualAddr *actual_address) {
	ASSERT((length % kPageSize) == 0);

	Mapping *mapping;
	if((flags & kMapFixed) != 0) {
		ASSERT((address % kPageSize) == 0);
		mapping = allocateAt(address, length);
	}else{
		mapping = allocate(length, flags);
	}
	ASSERT(mapping != nullptr);

	mapping->type = Mapping::kTypeMemory;
	mapping->memoryRegion = traits::move(memory);

	for(size_t i = 0; i < length / kPageSize; i++) {
		PhysicalAddr physical = memory->getPage(i);
		VirtualAddr vaddr = mapping->baseAddress + i * kPageSize;

		uint32_t page_flags = 0;

		constexpr uint32_t mask = kMapReadOnly | kMapReadExecute | kMapReadWrite;
		if((flags & mask) == kMapReadWrite) {
			page_flags |= PageSpace::kAccessWrite;
		}else if((flags & mask) == kMapReadExecute) {
			page_flags |= PageSpace::kAccessExecute;
		}else{
			ASSERT((flags & mask) == kMapReadOnly);
		}
		p_pageSpace.mapSingle4k(vaddr, physical, true, page_flags);
	}

	*actual_address = mapping->baseAddress;
}

void AddressSpace::switchTo() {
	p_pageSpace.switchTo();
}

Mapping *AddressSpace::getMapping(VirtualAddr address) {
	Mapping *current = p_root;

	while(current != nullptr) {
		if(address < current->baseAddress) {
			current = current->leftPtr;
		}else if(address >= current->baseAddress + current->length) {
			current = current->rightPtr;
		}else{
			ASSERT(address >= current->baseAddress
					&& address < current->baseAddress + current->length);
			return current;
		}
	}

	return nullptr;
}

Mapping *AddressSpace::allocate(size_t length, MapFlags flags) {
	ASSERT((length % kPageSize) == 0);

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
		
		if(mapping->leftPtr != nullptr
				&& mapping->leftPtr->largestHole >= length)
			return allocateDfs(mapping->leftPtr, length, flags);
		
		ASSERT(mapping->rightPtr != nullptr
				&& mapping->rightPtr->largestHole >= length);
		return allocateDfs(mapping->rightPtr, length, flags);
	}else{
		// try to allocate memory at the top of the range
		ASSERT((flags & kMapPreferTop) != 0);
		if(mapping->type == Mapping::kTypeHole && mapping->length >= length)
			return splitHole(mapping, mapping->length - length, length);

		if(mapping->rightPtr != nullptr
				&& mapping->rightPtr->largestHole >= length)
			return allocateDfs(mapping->rightPtr, length, flags);
		
		ASSERT(mapping->leftPtr != nullptr
				&& mapping->leftPtr->largestHole >= length);
		return allocateDfs(mapping->leftPtr, length, flags);
	}
}

Mapping *AddressSpace::allocateAt(VirtualAddr address, size_t length) {
	ASSERT((address % kPageSize) == 0);
	ASSERT((length % kPageSize) == 0);

	Mapping *hole = getMapping(address);
	ASSERT(hole != nullptr);
	ASSERT(hole->type == Mapping::kTypeHole);
	
	return splitHole(hole, address - hole->baseAddress, length);
}

Mapping *AddressSpace::splitHole(Mapping *mapping,
		VirtualAddr split_offset, size_t split_length) {
	ASSERT(split_length > 0);
	ASSERT(mapping->type == Mapping::kTypeHole);
	ASSERT(split_offset + split_length <= mapping->length);
	
	Mapping *lower = mapping->lowerPtr;
	Mapping *higher = mapping->higherPtr;
	VirtualAddr hole_address = mapping->baseAddress;
	size_t hole_length = mapping->length;

	auto split = frigg::memory::construct<Mapping>(*kernelAlloc, Mapping::kTypeNone,
			hole_address + split_offset, split_length);
	
	if(split_offset == 0) {
		// the split mapping starts at the beginning of the hole
		// we have to delete the hole mapping
		split->lowerPtr = lower;
		if(lower != nullptr)
			lower->higherPtr = split;

		addressTreeRemove(mapping);

		frigg::memory::destruct(*kernelAlloc, mapping);
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
		auto following = frigg::memory::construct<Mapping>(*kernelAlloc, Mapping::kTypeHole,
				hole_address + split_offset + split_length,
				hole_length - split_offset - split_length);
		split->higherPtr = following;
		following->lowerPtr = split;
		following->higherPtr = higher;
		if(higher != nullptr)
			higher->lowerPtr = following;

		addressTreeInsert(following);
	}else{
		ASSERT(hole_length == split_offset + split_length);

		// the split mapping goes on until the end of the hole
		split->higherPtr = higher;
		if(higher != nullptr)
			higher->lowerPtr = split;
	}

	return split;
}

void AddressSpace::rotateLeft(Mapping *n) {
	Mapping *u = n->parentPtr;
	ASSERT(u != nullptr && u->rightPtr == n);
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
		ASSERT(w->rightPtr == u);
		w->rightPtr = n;
	}
}

void AddressSpace::rotateRight(Mapping *n) {
	Mapping *u = n->parentPtr;
	ASSERT(u != nullptr && u->leftPtr == n);
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
		ASSERT(w->rightPtr == u);
		w->rightPtr = n;
	}
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
	if(p_root == nullptr) {
		p_root = mapping;

		fixAfterInsert(mapping);
		return;
	}

	Mapping *current = p_root;

	while(true) {
		if(mapping->baseAddress < current->baseAddress) {
			if(current->leftPtr == nullptr) {
				current->leftPtr = mapping;
				mapping->parentPtr = current;

				updateLargestHole(mapping);

				fixAfterInsert(mapping);
				return;
			}else{
				current = current->leftPtr;
			}
		}else{
			ASSERT(mapping->baseAddress > current->baseAddress);
			if(current->rightPtr == nullptr) {
				current->rightPtr = mapping;
				mapping->parentPtr = current;
				
				updateLargestHole(mapping);
				
				fixAfterInsert(mapping);
				return;
			}else{
				current = current->rightPtr;
			}
		}
	}
}

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
	ASSERT(grand != nullptr);
	
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
		if(n == parent->rightPtr)
			rotateLeft(n);

		rotateRight(parent);
		parent->color = Mapping::kColorBlack;
		grand->color = Mapping::kColorRed;

		fixAfterInsert(grand);
	}else if(parent == grand->rightPtr) {
		if(n == parent->leftPtr)
			rotateRight(n);

		rotateLeft(parent);
		parent->color = Mapping::kColorBlack;
		grand->color = Mapping::kColorRed;

		fixAfterInsert(grand);
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
		}else{
			ASSERT(mapping == parent->rightPtr);
			parent->rightPtr = right;
		}
		if(right) {
			right->parentPtr = parent;
			
			if(mapping->color == Mapping::kColorBlack) {
				if(right->color == Mapping::kColorRed) {
					right->color = Mapping::kColorBlack;
				}else{
					fixAfterRemove(right);
				}
			}
		}
	}else if(mapping->rightPtr == nullptr) {
		// replace the mapping by its left child
		if(parent == nullptr) {
			p_root = left;
		}else if(mapping == parent->leftPtr) {
			parent->leftPtr = left;
		}else{
			ASSERT(mapping == parent->rightPtr);
			parent->rightPtr = left;
		}
		if(left) {
			left->parentPtr = parent;

			if(mapping->color == Mapping::kColorBlack) {
				if(left->color == Mapping::kColorRed) {
					left->color = Mapping::kColorBlack;
				}else{
					fixAfterRemove(left);
				}
			}
		}
	}else{
		// TODO: replace by mapping->lowerPtr
		Mapping *predecessor = mapping->leftPtr;
		while(predecessor->rightPtr != nullptr)
			predecessor = predecessor->rightPtr;
		ASSERT(predecessor == mapping->lowerPtr);

		// replace the predecessor by its left child
		Mapping *pre_parent = predecessor->parentPtr;
		Mapping *pre_replace = predecessor->leftPtr;
		if(predecessor == pre_parent->leftPtr) {
			pre_parent->leftPtr = pre_replace;
		}else{
			ASSERT(predecessor == pre_parent->rightPtr);
			pre_parent->rightPtr = pre_replace;
		}
		if(pre_replace) {
			pre_replace->parentPtr = pre_parent;
			
			if(predecessor->color == Mapping::kColorBlack) {
				if(pre_replace->color == Mapping::kColorRed) {
					pre_replace->color = Mapping::kColorBlack;
				}else{
					fixAfterRemove(pre_replace);
				}
			}
		}

		updateLargestHole(pre_parent);

		// replace the mapping by its predecessor
		if(parent == nullptr) {
			p_root = predecessor;
		}else if(mapping == parent->leftPtr) {
			parent->leftPtr = predecessor;
		}else{
			ASSERT(mapping == parent->rightPtr);
			parent->rightPtr = predecessor;
		}
		predecessor->leftPtr = left;
		left->parentPtr = predecessor;
		predecessor->rightPtr = right;
		right->parentPtr = predecessor;
		predecessor->parentPtr = parent;
		predecessor->color = mapping->color;
	}
	
	if(parent != nullptr)
		updateLargestHole(parent);
}

void AddressSpace::fixAfterRemove(Mapping *n) {
	Mapping *parent = n->parentPtr;
	if(parent == nullptr)
		return;
	
	// this will always be the sibling of our node
	Mapping *s;
	
	// rotate so that our node has a black sibling
	if(parent->leftPtr == n) {
		if(isRed(parent->rightPtr)) {
			rotateLeft(parent->rightPtr);
			
			parent->color = Mapping::kColorRed;
			parent->rightPtr->color = Mapping::kColorBlack;
		}
		
		s = parent->rightPtr;
	}else{
		ASSERT(parent->rightPtr == n);
		if(isRed(parent->leftPtr)) {
			rotateLeft(parent->leftPtr);
			
			parent->color = Mapping::kColorRed;
			parent->leftPtr->color = Mapping::kColorBlack;
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
		ASSERT(isRed(s->rightPtr));

		rotateLeft(s);
		parent->color = Mapping::kColorBlack;
		s->color = parent_color;
		s->rightPtr->color = Mapping::kColorBlack;
	}else{
		ASSERT(parent->rightPtr == n);

		// rotate so that s->leftPtr is red
		if(isRed(s->rightPtr) && isBlack(s->leftPtr)) {
			Mapping *child = s->rightPtr;
			rotateRight(child);

			s->color = Mapping::kColorRed;
			child->color = Mapping::kColorBlack;

			s = child;
		}
		ASSERT(isRed(s->leftPtr));

		rotateRight(s);
		parent->color = Mapping::kColorBlack;
		s->color = parent_color;
		s->leftPtr->color = Mapping::kColorBlack;
	}
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

