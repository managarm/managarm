
#include <hel.h>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>
#include <protocols/fs/file-locks.hpp>
#include <blockfs.hpp>

namespace blockfs {
namespace raw {

using FlockManager = protocols::fs::FlockManager;
using Flock = protocols::fs::Flock;

struct RawFs {
	RawFs(BlockDevice *device);

	async::result<void> init();

	async::detached manageMapping();

	BlockDevice *device;
	HelHandle backingMemory;
	HelHandle frontalMemory;
	helix::Mapping fileMapping;
	FlockManager flockManager;
};

struct OpenFile {
	OpenFile(RawFs *rawFs);

	RawFs *rawFs;
	uint64_t offset;
	Flock flock;
};

} // namespace raw
} // namespace blockfs
