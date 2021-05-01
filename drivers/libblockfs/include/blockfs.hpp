
#ifndef LIBFS_HPP
#define LIBFS_HPP

#include <async/result.hpp>

namespace blockfs {

struct BlockDevice {
	BlockDevice(size_t sector_size);

	virtual ~BlockDevice() = default;

	virtual async::result<void> readSectors(uint64_t sector, void *buffer,
			size_t num_sectors) = 0;

	virtual async::result<void> writeSectors(uint64_t, const void *, size_t) {
		throw std::runtime_error("BlockDevice does not support writeSectors()");
	}

	const size_t sectorSize;
};

async::detached runDevice(BlockDevice *device);

} // namespace blockfs

#endif // LIBFS_HPP

