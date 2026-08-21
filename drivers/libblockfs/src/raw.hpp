
#include <hel.h>
#include <async/mutex.hpp>
#include <frg/mutex.hpp>
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

	async::result<void> manageMapping();

	BlockDevice *device;
	HelHandle backingMemory;
	HelHandle frontalMemory;
	helix::Mapping fileMapping;
	FlockManager flockManager;
};

struct OpenFile {
	OpenFile(RawFs *rawFs);

	struct HandleIoctl;

	RawFs *rawFs;
	async::mutex offsetMutex;
	// Protected by offsetMutex.
	uint64_t offset = 0;
	Flock flock;
};

extern protocols::fs::FileOperations rawOperations;

} // namespace raw
} // namespace blockfs
