#pragma once

#include <async/result.hpp>
#include <protocols/ostrace/ostrace.hpp>
#include <stdint.h>

namespace blockfs {

struct BlockDevice {
	BlockDevice(size_t sector_size, int64_t parent_id);

	virtual ~BlockDevice() = default;

	virtual async::result<void> readSectors(uint64_t sector, void *buffer,
			size_t num_sectors) = 0;

	virtual async::result<void> writeSectors(uint64_t, const void *, size_t) {
		throw std::runtime_error("BlockDevice does not support writeSectors()");
	}

	virtual async::result<size_t> getSize() = 0;

	size_t size;
	const size_t sectorSize;
	const int64_t parentId;

protected:
};

async::detached runDevice(BlockDevice *device);

extern protocols::ostrace::Event ostEvtGetLink;
extern protocols::ostrace::Event ostEvtTraverseLinks;
extern protocols::ostrace::Event ostEvtRead;
extern protocols::ostrace::Event ostEvtRawRead;
extern protocols::ostrace::Event ostEvtExt2InitiateInode;
extern protocols::ostrace::Event ostEvtExt2ManageInode;
extern protocols::ostrace::Event ostEvtExt2ManageFile;
extern protocols::ostrace::UintAttribute ostAttrTime;
extern protocols::ostrace::UintAttribute ostAttrNumBytes;

extern protocols::ostrace::Context ostContext;

} // namespace blockfs
