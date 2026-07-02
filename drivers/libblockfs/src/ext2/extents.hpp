#pragma once

#include "ext2fs.hpp"

namespace blockfs::ext2fs {

struct ExtentWalkInfo {
	ExtentHeader *hdr;
	// null for the inode embedded level
	std::optional<uint64_t> block;
	arch::dma_buffer_view blockView;
	uint16_t index;

	// for non-leaf levels
	ExtentIndex *indices;
	// for leaf level
	Extent *extents;
};

enum class ExtentIterDecision {
	keepGoing,
	stop
};

struct ExtentWalker {
	ExtentWalker(FileSystem *fs, Inode *inode, bool lookup) : fs{fs}, inode{inode}, lookup{lookup} { }

	static constexpr uint16_t maxExtentDepthAfterInode = 4;

	/*
	 * blockIndex: The block index within the file to be searched/inserted in/to the extent tree.
	 * Enter: A callback accepting `const ExtentWalkInfo &` that is called before a new tree level is entered.
	 * Leave: A callback accepting `const ExtentWalkInfo &` that is called at the end going up the walked levels.
	 *
	 * If looking up a block returns a boolean indicating whether the block was found.
	 *
	 * Callers must hold inode->blockMapMutex.
	 */
	template<typename Enter, typename Leave>
	async::result<bool> walk(uint64_t blockIndex, Enter enter, Leave leave) {
		auto bufferBytesPerBlock = fs->sectorsPerBlock * fs->device->sectorSize;
		arch::dma_buffer blockBuffer{fs->pool, bufferBytesPerBlock * maxExtentDepthAfterInode};
		arch::dma_buffer_view subview;

		auto hdr = &inode->diskInode()->data.extents.hdr;
		std::optional<uint64_t> currentBlock;

		ExtentWalkInfo path[5];
		int pathCount = 0;

		while(hdr->depth != 0) {
			assert(hdr->magic == EXT4_EXTENT_MAGIC);

			auto indices = reinterpret_cast<ExtentIndex *>(&hdr[1]);

			uint32_t prevIndex = 0;

			assert(hdr->entries);

			for(uint32_t i = 0; i < hdr->entries; i++) {
				auto &idx = indices[i];
				if(idx.block > blockIndex) {
					break;
				}

				prevIndex = i;
			}

			if(lookup) {
				if(indices[prevIndex].block > blockIndex)
					co_return false;
			}

			// There should be at most 3 intermediate levels in a valid ext4 extent tree.
			assert(pathCount < 3);
			path[pathCount++] = {
				.hdr = hdr,
				.block = currentBlock,
				.blockView = subview,
				.index = static_cast<uint16_t>(prevIndex),
				.indices = indices,
				.extents = nullptr
			};

			co_await enter(path[pathCount - 1]);

			auto &idx = indices[prevIndex];
			uint64_t nextBlock = static_cast<uint64_t>(idx.leafLow)
					| (static_cast<uint64_t>(idx.leafHigh) << 32);

			subview = blockBuffer.subview((pathCount - 1) * bufferBytesPerBlock, bufferBytesPerBlock);
			co_await fs->device->readSectors(nextBlock * fs->sectorsPerBlock, subview);
			currentBlock = nextBlock;
			hdr = reinterpret_cast<ExtentHeader *>(subview.data());
		}

		assert(hdr->magic == EXT4_EXTENT_MAGIC);

		auto extents = reinterpret_cast<Extent *>(&hdr[1]);

		uint32_t prevIndex = 0;

		for(uint16_t i = 0; i < hdr->entries; i++) {
			auto &ext = extents[i];
			if(ext.block > blockIndex) {
				break;
			}

			prevIndex = i;
		}

		if(lookup) {
			if(!hdr->entries)
				co_return false;

			auto &extent = extents[prevIndex];
			if(extent.block > blockIndex || blockIndex - extent.block >= extent.len)
				co_return false;
		}

		path[pathCount++] = {
			.hdr = hdr,
			.block = currentBlock,
			.blockView = subview,
			.index = static_cast<uint16_t>(prevIndex),
			.indices = nullptr,
			.extents = extents
		};

		co_await enter(path[pathCount - 1]);

		for(int i = pathCount; i > 0; i--)
			if(co_await leave(path[i - 1]) == ExtentIterDecision::stop)
				break;

		co_return true;
	}

	FileSystem *fs;
	Inode *inode;
	bool lookup;
};

} // namespace blockfs:.ext2fs
