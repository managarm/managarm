
#ifndef LIBFS_HPP
#define LIBFS_HPP

#include <cofiber.hpp>
#include <cofiber/future.hpp>

namespace blockfs {

struct BlockDevice {
	BlockDevice(size_t sector_size);

	virtual cofiber::future<void> readSectors(uint64_t sector, void *buffer,
			size_t num_sectors) = 0;

	const size_t sectorSize;
};

cofiber::no_future runDevice(BlockDevice *device);

} // namespace blockfs

#endif // LIBFS_HPP

