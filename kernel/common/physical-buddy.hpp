#pragma once

#include <stddef.h>
#include <stdint.h>
#include <assert.h>

#include <frg/optional.hpp>

struct BuddyAccessor {
	using AddressType = uint64_t;

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

	BuddyAccessor(AddressType baseAddress, int sizeShift,
			int8_t *buddyPointer, AddressType numRoots, int tableOrder_)
	: _baseAddress{baseAddress}, _sizeShift{sizeShift},
			buddyPointer_{buddyPointer}, numRoots_{numRoots}, tableOrder_{tableOrder_} { }

	int tableOrder() { return tableOrder_; }

	AddressType allocate(int order, int addressBits) {
		assert(order >= 0);
		if(order > tableOrder_)
			return illegalAddress;

		int currentOrder = tableOrder_;
		int8_t *slice = buddyPointer_;

		// Note: the calculations here need to carefully take overflows into account!
		//       Hence, first exclude the case that the entire buddy tree is accessible.
		AddressType eligibleRoots = numRoots_;
		if(addressBits < static_cast<int>(sizeof(AddressType) * 8)) {
			if(_baseAddress >= (AddressType{1} << addressBits))
				return illegalAddress;

			// Range of memory that can be addressed;
			// starts at _baseAddress but is not necessarily fully contained in the buddy.
			AddressType addressableRange = (AddressType{1} << addressBits) - _baseAddress;
			if(addressableRange < (AddressType{1} << (order + _sizeShift)))
				return illegalAddress;

			// Descend until some constant number (i.e., 4) of roots are fully addressable,
			// i.e., are addressable with addressBits-many bits.
			while(currentOrder > order
					&& (addressableRange >> (currentOrder + _sizeShift)) > 4) {
				slice += size_t(numRoots_) << (tableOrder_ - currentOrder);
				currentOrder--;
			}
			eligibleRoots = addressableRange >> (currentOrder + _sizeShift);
			assert(eligibleRoots);
		}

		// First phase: Descent to the target order.
		// In this phase find a free element.
		AddressType allocIndex = findAllocatableChunk(slice, 0, eligibleRoots, order);
		if(allocIndex == illegalAddress)
			return illegalAddress;
		while(currentOrder > order) {
			slice += size_t(numRoots_) << (tableOrder_ - currentOrder);
			currentOrder--;
			allocIndex = findAllocatableChunk(slice, 2 * allocIndex, 2, order);
			if(allocIndex == illegalAddress)
				return illegalAddress;
		}

		// Here we perform the actual allocation.
		assert(slice[allocIndex] == order);
		slice[allocIndex] = -1;

		// Second phase: Ascent to the tableOrder.
		// In this phase we fix all superior elements.
		AddressType updateIndex = allocIndex;
		while(currentOrder < tableOrder_) {
			updateIndex /= 2;
			auto freeOrder = scanFreeChunks(slice, 2 * updateIndex, 2);
			currentOrder++;
			slice -= size_t(numRoots_) << (tableOrder_ - currentOrder);
			slice[updateIndex] = freeOrder;
		}

		AddressType physical = _baseAddress + (allocIndex << (order + _sizeShift));
		if(addressBits < static_cast<int>(sizeof(AddressType) * 8))
			assert(!(physical >> addressBits));
		return physical;
	}

	void free(AddressType address, int order) {
		assert(address >= _baseAddress);
		assert(order >= 0 && order <= tableOrder_);

		AddressType index = (address - _baseAddress) >> _sizeShift;
		assert(index % (size_t(1) << order) == 0);

		int currentOrder = tableOrder_;
		int8_t *slice = buddyPointer_;

		// Analogous to the allocate operation:
		// First we decend to the target order.
		while(currentOrder > order) {
			slice += size_t(numRoots_) << (tableOrder_ - currentOrder);
			currentOrder--;
		}

		// Perform the actual free operation.
		AddressType updateIndex = index >> order;
		assert(slice[updateIndex] == -1);
		slice[updateIndex] = order;

		// Update all superior elements.
		while(currentOrder < tableOrder_) {
			updateIndex /= 2;
			auto freeOrder = scanFreeChunks(slice, 2 * updateIndex, 2);
			currentOrder++;
			slice -= size_t(numRoots_) << (tableOrder_ - currentOrder);
			slice[updateIndex] = freeOrder;
		}
	}

private:
	AddressType _baseAddress;
	int _sizeShift;
	int8_t *buddyPointer_;
	AddressType numRoots_;
	int tableOrder_;
};
