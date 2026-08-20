#include <bit>
#include <string.h>
#include <utility>

#include <frg/mutex.hpp>
#include <helix/dispatcher-pool.hpp>

#include "metadata-cache.hpp"

namespace blockfs {

namespace {
	constexpr uint32_t pageShift = 12;
	// Number of blocks that stay locked and mapped after their last BlockWindow is gone.
	constexpr size_t lruCapacity = 128;
}

MetadataCache::MetadataCache(BlockDevice *device, uint64_t numBlocks, size_t blockSize)
: device_{device}, numBlocks_{numBlocks}, blockSize_{blockSize} {
	assert(std::has_single_bit(blockSize));
	assert(blockSize >= device->sectorSize);
	auto blockShift = static_cast<uint32_t>(std::countr_zero(blockSize));
	blockPagesShift_ = blockShift < pageShift ? pageShift : blockShift;
	sectorsPerBlock_ = blockSize / device->sectorSize;

	HelHandle backing, frontal;
	HEL_CHECK(helCreateManagedMemory(numBlocks << blockPagesShift_,
			0, &backing, &frontal));
	frontal_ = helix::UniqueDescriptor{frontal};

	manage_(helix::UniqueDescriptor{backing});
	flushDirty_();
}

void MetadataCache::CacheBlockPolicy::increment() const {
	cacheBlock_->refCount.increment();
}

void MetadataCache::CacheBlockPolicy::decrement() const {
	if(cacheBlock_->refCount.decrement_and_check_if_zero())
		cacheBlock_->cache->destroyCacheBlock_(cacheBlock_);
}

void MetadataCache::BlockWindow::markDirty() {
	assert(cacheBlock_);
	cacheBlock_->cache->markDirty_(cacheBlock_);
}

MetadataCache::CacheBlockPtr MetadataCache::findCacheBlock_(uint64_t block) {
	CacheBlockPtr evicted; // Drop after the lock.
	CacheBlockPtr cacheBlock;
	{
		std::lock_guard cacheBlockLock{cacheBlockMutex_};

		auto it = cacheBlocks_.find(block);
		if(it == cacheBlocks_.end())
			return nullptr;
		// The entry may refer to a cache block that already expired but did not erase it yet.
		if(!it->second->refCount.increment_if_nonzero())
			return nullptr;
		cacheBlock = CacheBlockPtr{smarter::adopt_rc, it->second, CacheBlockPolicy{it->second}};
		evicted = touchLru_(cacheBlock.get());
	}
	return cacheBlock;
}

// Moves the cache block to the front of the LRU.
// Returns a cache block evicted to make room, if any.
// The caller must drop that reference only after releasing cacheBlockMutex_.
MetadataCache::CacheBlockPtr MetadataCache::touchLru_(CacheBlock *cacheBlock) {
	if(cacheBlock->inLru) {
		lru_.erase(lru_.iterator_to(cacheBlock));
		lru_.push_front(cacheBlock);
		return nullptr;
	}
	// LRU owns a reference to the cache block.
	cacheBlock->refCount.increment();
	lru_.push_front(cacheBlock);
	cacheBlock->inLru = true;
	if(++lruSize_ <= lruCapacity)
		return nullptr;
	auto *evicted = lru_.pop_back();
	evicted->inLru = false;
	--lruSize_;
	return CacheBlockPtr{smarter::adopt_rc, evicted, CacheBlockPolicy{evicted}};
}

void MetadataCache::destroyCacheBlock_(CacheBlock *cacheBlock) {
	{
		std::lock_guard cacheBlockLock{cacheBlockMutex_};

		assert(!cacheBlock->inLru);
		// The entry may already refer to a replacement cache block that was created after this one expired.
		auto it = cacheBlocks_.find(cacheBlock->block);
		if(it != cacheBlocks_.end() && it->second == cacheBlock)
			cacheBlocks_.erase(it);
	}
	delete cacheBlock;
}

async::result<MetadataCache::BlockWindow> MetadataCache::access(uint64_t block, bool writable) {
	assert(block < numBlocks_);

	if(auto cacheBlock = findCacheBlock_(block))
		co_return BlockWindow{std::move(cacheBlock), writable};

	co_await creationMutex_.async_lock();
	frg::unique_lock creationLock{frg::adopt_lock, creationMutex_};

	if(auto cacheBlock = findCacheBlock_(block))
		co_return BlockWindow{std::move(cacheBlock), writable};

	auto frameSize = size_t{1} << blockPagesShift_;

	helix::LockMemoryView lockMemory;
	auto &&submit = helix::submitLockMemoryView(frontal_, &lockMemory,
			block << blockPagesShift_, frameSize, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lockMemory.error());

	helix::Mapping mapping{frontal_, static_cast<ptrdiff_t>(block << blockPagesShift_),
			frameSize, kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};

	auto cacheBlock = CacheBlock::create(this, block, lockMemory.descriptor(), std::move(mapping));
	CacheBlockPtr evicted; // Drop after the lock.
	{
		std::lock_guard cacheBlockLock{cacheBlockMutex_};
		cacheBlocks_[block] = cacheBlock.get();
		evicted = touchLru_(cacheBlock.get());
	}
	co_return BlockWindow{std::move(cacheBlock), writable};
}

async::result<void> MetadataCache::read(uint64_t block, size_t offset, size_t length, void *buffer) {
	assert(block < numBlocks_);
	assert(offset + length <= blockSize_);

	auto readMemory = co_await helix_ng::readMemory(frontal_,
			(block << blockPagesShift_) + offset, length, buffer);
	HEL_CHECK(readMemory.error());
}

async::detached MetadataCache::manage_(helix::UniqueDescriptor backing) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submitManage = helix::submitManageMemory(backing,
				&manage, helix::Dispatcher::global());
		co_await submitManage.async_wait();
		HEL_CHECK(manage.error());

		helix::DispatcherPool::global().detach(
				serviceRequest_(backing, manage.type(), manage.offset(), manage.length()));
	}
}

async::result<void> MetadataCache::serviceRequest_(helix::BorrowedDescriptor backing,
		int type, uintptr_t offset, size_t length) {
	auto frameSize = size_t{1} << blockPagesShift_;
	assert(!(offset & (frameSize - 1)));
	assert(!(length & (frameSize - 1)));

	auto view = device_->pagePool->importMemory(backing, offset, length);

	for(size_t progress = 0; progress < length; progress += frameSize) {
		auto block = (offset + progress) >> blockPagesShift_;
		assert(block < numBlocks_);

		auto subview = view.view().subview(progress, blockSize_);

		if(type == kHelManageInitialize) {
			co_await device_->readSectors(block * sectorsPerBlock_, subview);
			// Zero the tail of the frame that no disk block backs.
			if(blockSize_ < frameSize)
				memset(view.view().subview(progress + blockSize_,
						frameSize - blockSize_).data(), 0, frameSize - blockSize_);
		}else{
			assert(type == kHelManageWriteback);
			co_await device_->writeSectors(block * sectorsPerBlock_, subview);
		}
	}

	HEL_CHECK(helUpdateMemory(backing.getHandle(), type, offset, length));
}

void MetadataCache::markDirty_(CacheBlockPtr cacheBlock) {
	{
		std::lock_guard dirtyLock{dirtyMutex_};

		// An entry that is already queued is covered by the raise that queued it.
		if(!dirtyCacheBlocks_.insert(std::move(cacheBlock)).second)
			return;
	}

	dirtyEvent_.raise();
}

async::detached MetadataCache::flushDirty_() {
	// Cache blocks dirtied while a batch is being synchronized are picked up by the next iteration.
	uint64_t seenSeq = 0;
	std::vector<CacheBlockPtr> batch;
	while(true) {
		co_await dirtyEvent_.async_wait(seenSeq);

		{
			std::lock_guard dirtyLock{dirtyMutex_};

			// Take the sequence together with the batch, so that no request is missed.
			// TODO: Use async::sequenced_event::current_sequence() once it exists.
			seenSeq = dirtyEvent_.next_sequence() - 1;
			batch.assign(dirtyCacheBlocks_.begin(), dirtyCacheBlocks_.end());
			dirtyCacheBlocks_.clear();
		}

		for(auto &cacheBlock : batch) {
			auto synchronize = co_await helix_ng::synchronizeSpace(
					helix::BorrowedDescriptor{kHelNullHandle},
					cacheBlock->mapping.get(), cacheBlock->mapping.size());
			HEL_CHECK(synchronize.error());
		}

		// Drop the references that kept the mappings alive across the writeback.
		batch.clear();
	}
}

} // namespace blockfs
