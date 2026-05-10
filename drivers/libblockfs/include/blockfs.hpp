#pragma once

#include <async/result.hpp>
#include <arch/dma_pool.hpp>
#include <protocols/fs/common.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/ostrace/ostrace.hpp>
#include <stdint.h>

namespace blockfs {

struct BlockDevice {
	BlockDevice(size_t sector_size, int64_t parent_id, arch::contiguous_pool *pool);

	virtual ~BlockDevice() = default;

	virtual async::result<void> readSectors(uint64_t, arch::dma_buffer_view) = 0;

	virtual async::result<void> writeSectors(uint64_t, arch::dma_buffer_view) {
		throw std::runtime_error("BlockDevice does not support writeSectors()");
	}

	virtual async::result<size_t> getSize() = 0;

	virtual async::result<void> handleIoctl(managarm::fs::GenericIoctlRequest &req, helix::BorrowedDescriptor conversation) {
		std::cout << "\e[31m" "libblockfs: Unknown ioctl() message with ID "
				<< req.command() << "\e[39m" << std::endl;

		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
	}

	size_t size;
	const size_t sectorSize;
	int64_t parentId = -1;

	std::string diskNamePrefix = "sd";
	std::string diskNameSuffix = "";
	std::string partNameSuffix = "";

	arch::contiguous_pool *pagePool;
protected:
};

async::detached runDevice(BlockDevice *device);

extern protocols::ostrace::Event ostEvtGetLink;
extern protocols::ostrace::Event ostEvtTraverseLinks;
extern protocols::ostrace::Event ostEvtRead;
extern protocols::ostrace::Event ostEvtRawRead;
extern protocols::ostrace::Event ostEvtExt2AssignDataBlocks;
extern protocols::ostrace::Event ostEvtExt2InitiateInode;
extern protocols::ostrace::Event ostEvtExt2ManageInode;
extern protocols::ostrace::Event ostEvtExt2ManageInodeBitmap;
extern protocols::ostrace::Event ostEvtExt2ManageFile;
extern protocols::ostrace::Event ostEvtExt2ManageBlockBitmap;
extern protocols::ostrace::Event ostEvtExt2AllocateBlocks;
extern protocols::ostrace::Event ostEvtExt2AllocateInode;
extern protocols::ostrace::UintAttribute ostAttrTime;
extern protocols::ostrace::UintAttribute ostAttrNumBytes;

extern protocols::ostrace::Context ostContext;

} // namespace blockfs
