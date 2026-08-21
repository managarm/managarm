#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace blockfs {
namespace ext2fs {

// In-memory cache of already-resolved runs of one inode's block map, keyed by file block.
// Caches both actual mappings and holes.
struct BlockMapCache {
	struct Run {
		// Only meaningful if the run is not a hole.
		uint64_t diskBlock;
		// Number of blocks this run represents.
		uint64_t size;
		// Whether the run is a hole or not.
		bool hole;
	};

	// Resolves fileBlock against the cache, considering at most limit blocks.
	// Returns the cached run starting at fileBlock, or nullopt if it is not cached,
	// together with the number of blocks that the answer covers.
	// The latter matches the run's size on a hit.
	// On a miss, it equals the number of uncached blocks before the next cached run.
	std::pair<std::optional<Run>, uint64_t> probe(uint64_t fileBlock, uint64_t limit);

	// Inserts a run at fileBlock, replacing the cached runs that it overlaps.
	void insert(uint64_t fileBlock, Run run);

	// Inserts the disk blocks backing consecutive file blocks starting at fileBlock.
	void insertList(uint64_t fileBlock, const std::vector<uint32_t> &blocks);

private:
	// Callers must hold mutex_.
	void insertLocked_(uint64_t fileBlock, Run run);

	std::mutex mutex_;
	std::map<uint64_t, Run> runs_;
};

} // namespace ext2fs
} // namespace blockfs
