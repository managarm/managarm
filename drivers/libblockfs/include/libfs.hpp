
#ifndef LIBFS_HPP
#define LIBFS_HPP

#define LIBFS_BEGIN_PRIVATE _Pragma("GCC visibility push(hidden)")
#define LIBFS_END_PRIVATE _Pragma("GCC visibility pop")

#include <helx.hpp>

#include <frigg/callback.hpp>

namespace libfs {

struct BlockDevice {
	BlockDevice(size_t sector_size);

	virtual void readSectors(uint64_t sector, void *buffer,
			size_t num_sectors, frigg::CallbackPtr<void()> callback) = 0;

	const size_t sectorSize;
};

void runDevice(helx::EventHub &event_hub, BlockDevice *device);

} // namespace libfs

#endif // LIBFS_HPP

