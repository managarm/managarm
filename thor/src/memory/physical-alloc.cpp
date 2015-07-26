
#include "../../../frigg/include/types.hpp"
#include "../util/general.hpp"
#include "../runtime.hpp"
#include "../debug.hpp"
#include "physical-alloc.hpp"
#include "paging.hpp"

namespace thor {
namespace memory {

PhysicalChunkAllocator *tableAllocator;

// --------------------------------------------------------
// Chunk
// --------------------------------------------------------

size_t Chunk::numBytesInLevel(int level) {
	if(level == 0)
		return kBytesInRoot;
	return numBytesInLevel(level - 1) * kGranularity;
}
size_t Chunk::numEntriesInLevel(int level) {
	return kEntriesPerByte * numBytesInLevel(level);
}
size_t Chunk::offsetOfLevel(int level) {
	if(level == 0)
		return 0;
	return offsetOfLevel(level - 1) + numBytesInLevel(level - 1);
}

size_t Chunk::pagesPerEntry(int level) {
	if(level == treeHeight)
		return 1;
	return pagesPerEntry(level + 1) * kGranularity;
}
size_t Chunk::spacePerEntry(int level) {
	return pageSize * pagesPerEntry(level);
}

Chunk::Chunk(PhysicalAddr base_addr, size_t page_size, size_t num_pages)
		: baseAddress(base_addr), pageSize(page_size), numPages(num_pages),
			treeHeight(0), bitmapTree(nullptr) {
	// calculate the number of tree levels we need
	while(numEntriesInLevel(treeHeight) < num_pages)
		treeHeight++;
}

size_t Chunk::calcBitmapTreeSize() {
	size_t size = 0;
	for(int k = 0; k < treeHeight; k++)
		size += numBytesInLevel(k);
	return size;
}

void Chunk::setupBitmapTree(uint8_t *bitmap_tree) {
	bitmapTree = bitmap_tree;

	// mark everything as white
	uint8_t sample = 0;
	for(int j = 0; j < kEntriesPerByte; j++)
		sample |= kColorWhite << (j * kEntryShift);
	memset(bitmap_tree, sample, calcBitmapTreeSize());
	
	// mark trailing entries as black
	size_t num_entries = numEntriesInLevel(treeHeight);
	for(size_t entry = numPages; entry < num_entries; entry++)
		markBlackRecursive(treeHeight, entry);
}

void Chunk::markColor(int level, int entry_in_level, uint8_t color) {
	int byte_in_level = entry_in_level / kEntriesPerByte;
	int entry_in_byte = entry_in_level % kEntriesPerByte;
	
	size_t offset = offsetOfLevel(level);
	uint8_t byte = bitmapTree[offset + byte_in_level];
	byte &= ~(kEntryMask << (entry_in_byte * kEntryShift));
	byte |= color << (entry_in_byte * kEntryShift);
	bitmapTree[offset + byte_in_level] = byte;
}

void Chunk::checkNeighbors(int level, int entry_in_level,
		bool &all_white, bool &all_black_or_red, bool &all_red) {
	size_t node_in_level = entry_in_level / kGranularity;
	size_t bytes_per_node = kGranularity / kEntriesPerByte;
	
	size_t node_start_byte_in_level = node_in_level * bytes_per_node;
	size_t node_limit_byte_in_level = (node_in_level + 1) * bytes_per_node;

	all_white = true;
	all_black_or_red = true;
	all_red = true;

	size_t offset = offsetOfLevel(level);
	for(int i = node_start_byte_in_level; i < node_limit_byte_in_level; i++) {
		uint8_t byte = bitmapTree[offset + i];
		
		for(int j = 0; j < kEntriesPerByte; j++) {
			int entry = (byte >> (j * kEntryShift)) & kEntryMask;
			switch(entry) {
			case kColorWhite:
				all_black_or_red = false;
				all_red = false;
				break;
			case kColorBlack:
				all_white = false;
				all_red = false;
				break;
			case kColorRed:
				all_white = false;
				break;
			case kColorGray:
				all_white = false;
				all_black_or_red = false;
				all_red = false;
				break;
			default:
				ASSERT(!"Unexpected color");
			}
		}
	}
}

void Chunk::markGrayRecursive(int level, int entry_in_level) {
	markColor(level, entry_in_level, kColorGray);
	
	if(level == 0)
		return;
	
	markGrayRecursive(level - 1, entry_in_level / kGranularity);
}

void Chunk::markBlackRecursive(int level, int entry_in_level) {
	markColor(level, entry_in_level, kColorBlack);
	
	if(level == 0)
		return;
	
	bool all_white, all_black_or_red, all_red;
	checkNeighbors(level, entry_in_level,
			all_white, all_black_or_red, all_red);
	ASSERT(!all_white && !all_red);

	if(all_black_or_red) {
		markBlackRecursive(level - 1, entry_in_level / kGranularity);
	}else{
		markGrayRecursive(level - 1, entry_in_level / kGranularity);
	}
}

void Chunk::markWhiteRecursive(int level, int entry_in_level) {
	markColor(level, entry_in_level, kColorWhite);
	
	if(level == 0)
		return;
	
	bool all_white, all_black_or_red, all_red;
	checkNeighbors(level, entry_in_level,
			all_white, all_black_or_red, all_red);
	ASSERT(!all_black_or_red && !all_red);

	if(all_white) {
		markWhiteRecursive(level - 1, entry_in_level / kGranularity);
	}else{
		markGrayRecursive(level - 1, entry_in_level / kGranularity);
	}
}

PhysicalAddr allocateInLevel(Chunk *chunk, int level,
		size_t start_entry_in_level, size_t limit_entry_in_level) {
	size_t offset = Chunk::offsetOfLevel(level);
	
	// this is a simplification that allows us to only look at whole bytes
	ASSERT((start_entry_in_level % Chunk::kEntriesPerByte) == 0
			&& (limit_entry_in_level % Chunk::kEntriesPerByte) == 0);

	size_t start_byte_in_level = start_entry_in_level / Chunk::kEntriesPerByte;
	size_t limit_byte_in_level = limit_entry_in_level / Chunk::kEntriesPerByte;
	
	for(int i = start_byte_in_level; i < limit_byte_in_level; i++) {
		for(int j = 0; j < Chunk::kEntriesPerByte; j++) {
			size_t entry_in_level = i * Chunk::kEntriesPerByte + j;

			uint8_t byte = chunk->bitmapTree[offset + i];
			uint8_t entry = (byte >> (j * Chunk::kEntryShift)) & Chunk::kEntryMask;
			
			if(level == chunk->treeHeight) {
				if(entry == Chunk::kColorWhite) {
					chunk->markBlackRecursive(level, entry_in_level);

					return chunk->baseAddress + entry_in_level * chunk->spacePerEntry(level);
				}else{
					ASSERT(entry == Chunk::kColorBlack
							|| entry == Chunk::kColorRed);
					// just continue searching
				}
			}else{
				if(entry == Chunk::kColorWhite
						|| entry == Chunk::kColorGray) {
					return allocateInLevel(chunk, level + 1,
							entry_in_level * Chunk::kGranularity,
							(entry_in_level + 1) * Chunk::kGranularity);
				}else{
					ASSERT(entry == Chunk::kColorBlack
							|| entry == Chunk::kColorRed);
					// just continue searching
				}
			}
		}
	}

	return 0;
}

// --------------------------------------------------------
// PhysicalChunkAllocator
// --------------------------------------------------------

PhysicalChunkAllocator::PhysicalChunkAllocator(PhysicalAddr bootstrap_base,
		size_t bootstrap_length)
: p_bootstrapBase(bootstrap_base), p_bootstrapLength(bootstrap_length),
		p_bootstrapPtr(bootstrap_base), p_root(nullptr) { }

void PhysicalChunkAllocator::addChunk(PhysicalAddr chunk_base,
		size_t chunk_length) {
	ASSERT(chunk_base % 0x1000 == 0);
	ASSERT(chunk_length % 0x1000 == 0);
	Chunk *chunk = new (bootstrapAlloc(sizeof(Chunk), alignof(Chunk)))
			Chunk(chunk_base, 0x1000, chunk_length / 0x1000);
	
	void *tree_ptr = bootstrapAlloc(chunk->calcBitmapTreeSize(), 1);
	chunk->setupBitmapTree((uint8_t *)tree_ptr);

	ASSERT(p_root == nullptr);
	p_root = chunk;
}

void PhysicalChunkAllocator::bootstrap() {
	size_t num_pages = (p_bootstrapPtr - p_bootstrapBase) / 0x1000;
	if((p_bootstrapPtr - p_bootstrapBase) % 0x1000 != 0)
		num_pages++;
	
	ASSERT(p_bootstrapBase >= p_root->baseAddress);
	ASSERT(p_bootstrapPtr <= p_root->baseAddress
			+ p_root->pageSize * p_root->numPages);
	
	for(size_t i = 0; i < num_pages; i++)
		p_root->markBlackRecursive(p_root->treeHeight,
				(p_bootstrapBase - p_root->baseAddress) / 0x1000 + i);
}

PhysicalAddr PhysicalChunkAllocator::allocate(size_t num_pages) {
	ASSERT(num_pages == 1);
	return allocateInLevel(p_root, 0, 0, Chunk::numEntriesInLevel(0));
}

void PhysicalChunkAllocator::free(PhysicalAddr address) {
	ASSERT(address >= p_root->baseAddress);
	ASSERT(address < p_root->baseAddress
			+ p_root->pageSize * p_root->numPages);
	
	p_root->markWhiteRecursive(p_root->treeHeight,
			(address - p_root->baseAddress) / 0x1000);
}

void *PhysicalChunkAllocator::bootstrapAlloc(size_t length,
		size_t alignment) {
	p_bootstrapPtr += alignment - (p_bootstrapPtr % alignment);
	void *pointer = physicalToVirtual(p_bootstrapPtr);
	p_bootstrapPtr += length;
	ASSERT(p_bootstrapPtr <= p_bootstrapBase + p_bootstrapLength);
	
	return pointer;
}

}} // namespace thor::memory

