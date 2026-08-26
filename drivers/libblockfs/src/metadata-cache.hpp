#pragma once

#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <async/mutex.hpp>
#include <async/result.hpp>
#include <async/sequenced-event.hpp>
#include <frg/list.hpp>
#include <hel.h>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>
#include <smarter.hpp>

#include <blockfs.hpp>

namespace blockfs {

// Mount-wide page cache for filesystem metadata blocks, indexed by disk block number.
// The cache offset of a block is a fixed function of its block number alone;
// hence, servicing a page reads no mutable filesystem state (such as block maps).
// Each block occupies its own page-aligned frame, i.e., blocks smaller than a page
// are never packed into the same page (to keep writeback of a page confined to a
// single block).
//
// The whole cache is mapped once at construction; windows only pin blocks into
// the cache. Blocks must only be touched while a window pins them: a fault on a
// non-resident block would have to be serviced by this very process.
struct MetadataCache {
private:
	struct CacheBlock;

	// Cache blocks are reference counted in place.
	struct CacheBlockPolicy {
		CacheBlockPolicy() = default;

		explicit CacheBlockPolicy(CacheBlock *cacheBlock)
		: cacheBlock_{cacheBlock} { }

		explicit operator bool () const {
			return cacheBlock_;
		}

		void increment() const;
		void decrement() const;

	private:
		CacheBlock *cacheBlock_ = nullptr;
	};
	static_assert(smarter::rc_policy<CacheBlockPolicy>);

	using CacheBlockPtr = smarter::shared_ptr<CacheBlock, CacheBlockPolicy>;

	// All BlockWindows of a block share one CacheBlock (and hence one lock that pins the block into the cache).
	struct CacheBlock {
		// The returned pointer adopts the reference that refCount starts out with.
		static CacheBlockPtr create(MetadataCache *cache, uint64_t block,
				helix::UniqueDescriptor lock) {
			auto cacheBlock = new CacheBlock{cache, block, std::move(lock)};
			return CacheBlockPtr{smarter::adopt_rc, cacheBlock, CacheBlockPolicy{cacheBlock}};
		}

		MetadataCache *cache;
		uint64_t block;
		helix::UniqueDescriptor lock;
		smarter::counter refCount{smarter::adopt_rc, 1};
		// Protected by cacheBlockMutex_. The LRU holds a reference iff inLru.
		frg::default_list_hook<CacheBlock> lruHook;
		bool inLru = false;

	private:
		CacheBlock(MetadataCache *cache, uint64_t block, helix::UniqueDescriptor lock)
		: cache{cache}, block{block}, lock{std::move(lock)} { }
	};

public:
	// A locked view of a single metadata block through the cache's persistent mapping.
	// The block stays present in the cache for the lifetime of this object.
	//
	// Destroying a writable window marks it as dirty in the kernel's writeback machinery.
	// Callers that write through a short-lived window therefore need not request a writeback explicitly.
	// Callers that use long-lived windows can use markDirty(). The writeback is asynchronous in both cases.
	struct BlockWindow {
		friend void swap(BlockWindow &lhs, BlockWindow &rhs) {
			std::swap(lhs.cacheBlock_, rhs.cacheBlock_);
			std::swap(lhs.writable_, rhs.writable_);
		}

		BlockWindow() = default;

		BlockWindow(BlockWindow &&other)
		: BlockWindow{} {
			swap(*this, other);
		}

		BlockWindow &operator=(BlockWindow other) {
			swap(*this, other);
			return *this;
		}

		~BlockWindow() {
			if(!cacheBlock_)
				return;
			if(writable_)
				markDirty();
		}

		void *get();

		// Requests a writeback of the pages written through this window's block.
		void markDirty();

	private:
		friend struct MetadataCache;

		BlockWindow(CacheBlockPtr cacheBlock, bool writable)
		: cacheBlock_{std::move(cacheBlock)}, writable_{writable} { }

		CacheBlockPtr cacheBlock_;
		bool writable_ = false;
	};

	MetadataCache(BlockDevice *device, uint64_t numBlocks, size_t blockSize);

	// The servicer coroutine started by the constructor refers back to this object.
	MetadataCache(const MetadataCache &) = delete;
	MetadataCache &operator=(const MetadataCache &) = delete;

	// Locks the given block and returns a window into the persistent mapping.
	// Writes through a writable window are eventually written back to the block.
	async::result<BlockWindow> access(uint64_t block, bool writable);

	// Reads bytes from the given block through the cache without mapping it.
	async::result<void> read(uint64_t block, size_t offset, size_t length, void *buffer);

private:
	async::detached manage_(helix::UniqueDescriptor backing);
	async::result<void> serviceRequest_(helix::BorrowedDescriptor backing,
			int type, uintptr_t offset, size_t length);
	void *blockAddress_(uint64_t block) {
		return static_cast<std::byte *>(mapping_.get()) + (block << blockPagesShift_);
	}
	CacheBlockPtr findCacheBlock_(uint64_t block);
	CacheBlockPtr touchLru_(CacheBlock *cacheBlock);
	void destroyCacheBlock_(CacheBlock *cacheBlock);
	void markDirty_(uint64_t block);
	async::detached flushDirty_();

	BlockDevice *device_;
	uint64_t numBlocks_;
	size_t blockSize_;
	uint32_t blockPagesShift_;
	size_t sectorsPerBlock_;
	helix::UniqueDescriptor frontal_;
	// Persistent mapping of the whole cache.
	helix::Mapping mapping_;

	std::mutex cacheBlockMutex_;
	// One entry per block that is currently cached.
	// Entries are erased before their cache block is deleted, but their refcounts may already be zero.
	// Thus, lookup under cacheBlockMutex_ can always dereference the pointer and trying to acquire a shared_ptr may fail.
	// Protected by cacheBlockMutex_.
	std::unordered_map<uint64_t, CacheBlock *> cacheBlocks_;
	// Keeps the most recently accessed blocks pinned even while no BlockWindow refers to them.
	// Protected by cacheBlockMutex_.
	frg::intrusive_list<
		CacheBlock,
		frg::locate_member<CacheBlock, frg::default_list_hook<CacheBlock>, &CacheBlock::lruHook>
	> lru_;
	// Number of cache blocks on lru_, which does not track its own size.
	// Protected by cacheBlockMutex_.
	size_t lruSize_ = 0;
	// Serializes cache block creation so that concurrent misses on the same block cannot create two of them.
	async::mutex creationMutex_;

	std::mutex dirtyMutex_;
	// Blocks awaiting a deferred writeback.
	// Protected by dirtyMutex_.
	std::unordered_set<uint64_t> dirtyBlocks_;
	// markDirty() calls that hit an already queued block.
	// Protected by dirtyMutex_.
	uint64_t numRedundantDirty_ = 0;
	// Raised to request a writeback of dirtyBlocks_.
	async::sequenced_event dirtyEvent_;
};

inline void *MetadataCache::BlockWindow::get() {
	return cacheBlock_->cache->blockAddress_(cacheBlock_->block);
}

} // namespace blockfs
