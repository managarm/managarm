
#ifndef LIBFS_HPP
#define LIBFS_HPP

#include <async/result.hpp>
#include <cofiber.hpp>

namespace blockfs {

struct BlockDevice {
	BlockDevice(size_t sector_size);

	virtual async::result<void> readSectors(uint64_t sector, void *buffer,
			size_t num_sectors) = 0;

	virtual async::result<void> writeSectors(uint64_t sector, const void *buffer,
			size_t num_sectors) {
		throw std::runtime_error("BlockDevice does not support writeSectors()");
	}

	const size_t sectorSize;
};

cofiber::no_future runDevice(BlockDevice *device);

} // namespace blockfs

#endif // LIBFS_HPP

