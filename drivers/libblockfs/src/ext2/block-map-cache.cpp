#include "block-map-cache.hpp"

#include <algorithm>
#include <cassert>
#include <iterator>

namespace blockfs {
namespace ext2fs {

namespace {

// Whether the run at bBlock directly continues the run at aBlock,
// i.e. whether the two can be merged into one.
bool continues(uint64_t aBlock, const BlockMapCache::Run &a,
		uint64_t bBlock, const BlockMapCache::Run &b) {
	if(aBlock + a.size != bBlock)
		return false;
	// Holes merge with adjacent holes.
	if(a.hole || b.hole)
		return a.hole && b.hole;
	// Non-holes merge only if they are contiguous on disk.
	return a.diskBlock + a.size == b.diskBlock;
}

// Drops the first skip blocks of a run.
BlockMapCache::Run advance(BlockMapCache::Run run, uint64_t skip) {
	assert(skip < run.size);
	if(!run.hole)
		run.diskBlock += skip;
	run.size -= skip;
	return run;
}

} // anonymous namespace

std::pair<std::optional<BlockMapCache::Run>, uint64_t>
BlockMapCache::probe(uint64_t fileBlock, uint64_t limit) {
	std::lock_guard cacheGuard{mutex_};

	auto it = runs_.upper_bound(fileBlock);
	if(it != runs_.begin()) {
		auto pred = std::prev(it);
		if(pred->first + pred->second.size > fileBlock) {
			auto run = advance(pred->second, fileBlock - pred->first);
			run.size = std::min(run.size, limit);
			return {run, run.size};
		}
	}

	// Cache miss; the gap extends up to the next cached run.
	if(it != runs_.end())
		limit = std::min(limit, it->first - fileBlock);
	return {std::nullopt, limit};
}

void BlockMapCache::insertLocked_(uint64_t fileBlock, Run run) {
	assert(run.size);
	auto end = fileBlock + run.size;

	// Trim (and possibly split) a preceding run that overlaps the start of the new run.
	auto it = runs_.lower_bound(fileBlock);
	if(it != runs_.begin()) {
		auto pred = std::prev(it);
		auto predEnd = pred->first + pred->second.size;
		if(predEnd > fileBlock) {
			if(predEnd > end)
				runs_.emplace(end, advance(pred->second, end - pred->first));
			pred->second.size = fileBlock - pred->first;
		}
	}

	// Remove (and possibly trim) runs that start inside the new run.
	while(it != runs_.end() && it->first < end) {
		if(it->first + it->second.size > end) {
			auto tail = advance(it->second, end - it->first);
			runs_.erase(it);
			runs_.emplace(end, tail);
			break;
		}
		it = runs_.erase(it);
	}

	// Insert the new run, merging with contiguous neighbors.
	auto succ = runs_.lower_bound(fileBlock);
	if(succ != runs_.end() && continues(fileBlock, run, succ->first, succ->second)) {
		run.size += succ->second.size;
		runs_.erase(succ);
	}

	auto next = runs_.lower_bound(fileBlock);
	if(next != runs_.begin()) {
		auto pred = std::prev(next);
		if(continues(pred->first, pred->second, fileBlock, run)) {
			pred->second.size += run.size;
			return;
		}
	}
	runs_.emplace(fileBlock, run);
}

void BlockMapCache::insert(uint64_t fileBlock, Run run) {
	std::lock_guard cacheGuard{mutex_};
	insertLocked_(fileBlock, run);
}

void BlockMapCache::insertList(uint64_t fileBlock, const std::vector<uint32_t> &blocks) {
	std::lock_guard cacheGuard{mutex_};
	for(size_t i = 0; i < blocks.size(); i++)
		insertLocked_(fileBlock + i, Run{.diskBlock = blocks[i], .size = 1, .hole = false});
}

} // namespace ext2fs
} // namespace blockfs
