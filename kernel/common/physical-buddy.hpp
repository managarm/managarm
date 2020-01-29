#pragma once

// TODO: To make this file more robust, we need to include some header
//       that provides the assert() macro.

#include <stddef.h>
#include <stdint.h>

#include <frg/optional.hpp>

struct BuddyAccessor {
	using AddressType = size_t;

	static inline constexpr AddressType illegalAddress = static_cast<AddressType>(-1);

private:
	static AddressType findAllocatableChunk(int8_t *slice, AddressType base, AddressType limit,
			int target) {
		for(AddressType i = 0; i < limit; i++) {
			if(slice[base + i] >= target)
				return base + i;
		}
		return illegalAddress;
	}

	// Determines the largest free chunk in the given range.
	static int scanFreeChunks(int8_t *slice, AddressType base, AddressType limit) {
		int freeOrder = -1;
		for(AddressType i = 0; i < limit; i++) {
			if(slice[base + i] >= freeOrder)
				freeOrder = slice[base + i];
		}
		return freeOrder;
	}

public:
	// This function determines a suitable order based on the number of items.
	static int suitableOrder(AddressType num_items) {
		int order = 0;
		while(num_items / (AddressType(1) << order) > 64)
			order++;
		return order;
	}

	// Determines the size required for the buddy allocator in bytes.
	static size_t determineSize(AddressType numRoots, int tableOrder) {
		size_t size = 0;
		for(int order = 0; order < tableOrder; order++)
			size += size_t(numRoots) << (tableOrder - order);
		return size;
	}

	// Inititalizes the buddy allocator array.
	static void initialize(int8_t *pointer, AddressType numRoots, int tableOrder) {
		int8_t *slice = pointer;
		for(int order = tableOrder; order >= 0; order--) {
			auto chunksInOrder = size_t(numRoots) << (tableOrder - order);
			for(AddressType i = 0; i < chunksInOrder; i++)
				slice[i] = order;
			slice += chunksInOrder;
		}
	}

	BuddyAccessor()
	: buddyPointer_{nullptr}, numRoots_{0}, tableOrder_{0} { }

	BuddyAccessor(int8_t *buddyPointer, AddressType numRoots, int tableOrder_)
	: buddyPointer_{buddyPointer}, numRoots_{numRoots}, tableOrder_{tableOrder_} { }

	AddressType allocate(int target) {
		assert(target >= 0 && target <= tableOrder_);

		int order = tableOrder_;
		int8_t *slice = buddyPointer_;

		// First phase: Descent to the target order.
		// In this phase find a free element.
		AddressType allocIndex = findAllocatableChunk(slice, 0, numRoots_, target);
		if(allocIndex == illegalAddress)
			return illegalAddress;
		while(order > target) {
			slice += size_t(numRoots_) << (tableOrder_ - order);
			order--;
			allocIndex = findAllocatableChunk(slice, 2 * allocIndex, 2, target);
			if(allocIndex == illegalAddress)
				return illegalAddress;
		}

		// Here we perform the actual allocation.
		assert(slice[allocIndex] == target);
		slice[allocIndex] = -1;

		// Second phase: Ascent to the tableOrder.
		// In this phase we fix all superior elements.
		AddressType updateIndex = allocIndex;
		while(order < tableOrder_) {
			updateIndex /= 2;
			auto freeOrder = scanFreeChunks(slice, 2 * updateIndex, 2);
			order++;
			slice -= size_t(numRoots_) << (tableOrder_ - order);
			slice[updateIndex] = freeOrder;
		}

		return allocIndex << target;
	}

	void free(AddressType address, int target) {
		assert(target >= 0 && target <= tableOrder_);
		assert(address % (size_t(1) << target) == 0);

		int order = tableOrder_;
		int8_t *slice = buddyPointer_;

		// Analogous to the allocate operation:
		// First we decend to the target order.
		while(order > target) {
			slice += size_t(numRoots_) << (tableOrder_ - order);
			order--;
		}

		// Perform the actual free operation.
		AddressType updateIndex = address >> target;
		assert(slice[updateIndex] == -1);
		slice[updateIndex] = target;

		// Update all superior elements.
		while(order < tableOrder_) {
			updateIndex /= 2;
			auto freeOrder = scanFreeChunks(slice, 2 * updateIndex, 2);
			order++;
			slice -= size_t(numRoots_) << (tableOrder_ - order);
			slice[updateIndex] = freeOrder;
		}
	}

private:
	int8_t *buddyPointer_;
	AddressType numRoots_;
	int tableOrder_;
};
