#include <bit>
#include <string.h>
#include <utility>

#include <frg/mutex.hpp>
#include <frg/scope_exit.hpp>
#include <helix/dispatcher-pool.hpp>

#include "metadata-cache.hpp"
#include "service-budget.hpp"
#include "trace.hpp"

namespace blockfs {

namespace {
	constexpr uint32_t pageShift = 12;
	// Number of blocks that stay locked and mapped after their last BlockWindow is gone.
	constexpr size_t lruCapacity = 128;
}

MetadataCache::MetadataCache(BlockDevice *device, uint64_t baseBlock, uint64_t numBlocks,
		size_t blockSize)
: device_{device}, baseBlock_{baseBlock}, numBlocks_{numBlocks}, blockSize_{blockSize} {
	assert(std::has_single_bit(blockSize));
	assert(blockSize >= device->sectorSize);
	auto blockShift = static_cast<uint32_t>(std::countr_zero(blockSize));
	blockPagesShift_ = blockShift < pageShift ? pageShift : blockShift;
	sectorsPerBlock_ = blockSize / device->sectorSize;

	HelHandle backing, frontal;
	HEL_CHECK(helCreateManagedMemory(numBlocks << blockPagesShift_,
			0, &backing, &frontal));
	frontal_ = helix::UniqueDescriptor{frontal};
	backing_ = helix::UniqueDescriptor{backing};

	// The mapping is lazy, i.e., its cost does not scale with the cache size.
	mapping_ = helix::Mapping{frontal_, 0,
			static_cast<size_t>(numBlocks << blockPagesShift_),
			kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};

	// Distribute MetadataCache servicing coroutines over the helix::DispatcherPool,
	// but keep manage_() and flushDirty_() on the same thread.
	helix::DispatcherPool::global().detach(run_());
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
	cacheBlock_->cache->markDirty_(cacheBlock_->block);
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
	protocols::ostrace::Timer timer;
	auto block = cacheBlock->block;

	{
		std::lock_guard cacheBlockLock{cacheBlockMutex_};

		assert(!cacheBlock->inLru);
		// The entry may already refer to a replacement cache block that was created after this one expired.
		auto it = cacheBlocks_.find(cacheBlock->block);
		if(it != cacheBlocks_.end() && it->second == cacheBlock)
			cacheBlocks_.erase(it);
	}
	delete cacheBlock;

	ostContext.emit(
		ostEvtMetadataUnmap,
		ostAttrTime(timer.elapsed()),
		ostAttrBlock(block)
	);
}

async::result<MetadataCache::BlockWindow> MetadataCache::access(uint64_t block, bool writable) {
	assert(block >= baseBlock_ && block - baseBlock_ < numBlocks_);

	protocols::ostrace::Timer timer;
	uint64_t timePin = 0;
	bool found = true;
	frg::scope_exit evtOnExit{[&] {
		ostContext.emit(
			ostEvtMetadataAccess,
			ostAttrTime(timer.elapsed()),
			ostAttrBlock(block),
			ostAttrFound(found),
			ostAttrTimePin(timePin)
		);
	}};

	if(auto cacheBlock = findCacheBlock_(block))
		co_return BlockWindow{std::move(cacheBlock), writable};

	co_await creationMutex_.async_lock();
	frg::unique_lock creationLock{frg::adopt_lock, creationMutex_};

	if(auto cacheBlock = findCacheBlock_(block))
		co_return BlockWindow{std::move(cacheBlock), writable};

	found = false;
	auto frameSize = size_t{1} << blockPagesShift_;

	helix::LockMemoryView lockMemory;
	auto &&submit = helix::submitLockMemoryView(frontal_, &lockMemory,
			blockOffset_(block), frameSize, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lockMemory.error());
	timePin = timer.split();

	auto cacheBlock = CacheBlock::create(this, block, lockMemory.descriptor());
	CacheBlockPtr evicted; // Drop after the lock.
	{
		std::lock_guard cacheBlockLock{cacheBlockMutex_};
		cacheBlocks_[block] = cacheBlock.get();
		evicted = touchLru_(cacheBlock.get());
	}
	co_return BlockWindow{std::move(cacheBlock), writable};
}

async::result<void> MetadataCache::read(uint64_t block, size_t offset, size_t length, void *buffer) {
	assert(block >= baseBlock_ && block - baseBlock_ < numBlocks_);
	assert(offset + length <= blockSize_);

	protocols::ostrace::Timer timer;

	auto readMemory = co_await helix_ng::readMemory(frontal_,
			blockOffset_(block) + offset, length, buffer);
	HEL_CHECK(readMemory.error());

	ostContext.emit(
		ostEvtMetadataRead,
		ostAttrTime(timer.elapsed()),
		ostAttrBlock(block),
		ostAttrNumBytes(length)
	);
}

async::result<void> MetadataCache::forget(uint64_t block) {
	assert(block >= baseBlock_ && block - baseBlock_ < numBlocks_);

	// The block is being freed, so its contents are of no interest anymore.
	{
		std::lock_guard dirtyLock{dirtyMutex_};
		dirtyBlocks_.erase(block);
	}

	// Destroy the CacheBlock object pointing to the block to unpin the block's memory.
	// If the block was still pinned, invalidateMemory() below would block on unpin.
	{
		CacheBlockPtr evicted; // Drop after the lock.
		{
			std::lock_guard cacheBlockLock{cacheBlockMutex_};

			auto it = cacheBlocks_.find(block);
			// Only the LRU may still reference the block: a live BlockWindow would keep
			// it pinned and hang invalidateMemory() below.
			assert(it == cacheBlocks_.end()
					|| it->second->refCount.check_count() <= (it->second->inLru ? 1u : 0u));
			if(it != cacheBlocks_.end() && it->second->inLru) {
				auto *cacheBlock = it->second;
				lru_.erase(lru_.iterator_to(cacheBlock));
				cacheBlock->inLru = false;
				--lruSize_;
				evicted = CacheBlockPtr{smarter::adopt_rc, cacheBlock,
						CacheBlockPolicy{cacheBlock}};
			}
		}
	}

	// Discard the page.
	auto frameSize = size_t{1} << blockPagesShift_;
	auto invalidate = co_await helix_ng::invalidateMemory(helix::BorrowedDescriptor{backing_},
			blockOffset_(block), frameSize, kHelInvalidateNoWriteback);
	HEL_CHECK(invalidate.error());
}

async::result<void> MetadataCache::run_() {
	co_await async::when_all(
		flushDirty_(),
		manage_()
	);
}

async::result<void> MetadataCache::manage_() {
	while(true) {
		helix::ManageMemory manage;
		auto &&submitManage = helix::submitManageMemory(helix::BorrowedDescriptor{backing_},
				&manage, helix::Dispatcher::global());
		co_await submitManage.async_wait();
		HEL_CHECK(manage.error());

		async::detach(serviceRequest_(helix::BorrowedDescriptor{backing_}, manage.type(),
				manage.offset(), manage.length()));
	}
}

async::result<void> MetadataCache::serviceRequest_(helix::BorrowedDescriptor backing,
		int type, uintptr_t offset, size_t length) {
	auto frameSize = size_t{1} << blockPagesShift_;
	assert(!(offset & (frameSize - 1)));
	assert(!(length & (frameSize - 1)));

	protocols::ostrace::Timer timer;
	uint64_t deviceTime = 0;

	// Acquire servicing budget before importMemory().
	auto budgetToken = co_await servicingBudget().acquire(type == kHelManageWriteback, length);
	auto budgetTime = timer.split();

	auto view = device_->pagePool->importMemory(backing, offset, length);
	auto importTime = timer.split();

	for(size_t progress = 0; progress < length; progress += frameSize) {
		auto block = baseBlock_ + ((offset + progress) >> blockPagesShift_);
		assert(block - baseBlock_ < numBlocks_);

		auto subview = view.view().subview(progress, blockSize_);

		protocols::ostrace::Timer deviceTimer;
		if(type == kHelManageInitialize) {
			co_await device_->readSectors(block * sectorsPerBlock_, subview);
			deviceTime += deviceTimer.elapsed();
			// Zero the tail of the frame that no disk block backs.
			if(blockSize_ < frameSize)
				memset(view.view().subview(progress + blockSize_,
						frameSize - blockSize_).data(), 0, frameSize - blockSize_);
		}else{
			assert(type == kHelManageWriteback);
			co_await device_->writeSectors(block * sectorsPerBlock_, subview);
			deviceTime += deviceTimer.elapsed();
		}
	}

	HEL_CHECK(helUpdateMemory(backing.getHandle(), type, offset, length));

	ostContext.emit(
		type == kHelManageInitialize ? ostEvtMetadataInitialize : ostEvtMetadataWriteback,
		ostAttrTime(timer.elapsed()),
		ostAttrNumBytes(length),
		ostAttrBlock(baseBlock_ + (offset >> blockPagesShift_)),
		ostAttrTimeBudget(budgetTime),
		ostAttrTimeImport(importTime),
		ostAttrTimeDevice(deviceTime)
	);
}

void MetadataCache::markDirty_(uint64_t block) {
	{
		std::lock_guard dirtyLock{dirtyMutex_};

		// An entry that is already queued is covered by the raise that queued it.
		if(!dirtyBlocks_.insert(block).second) {
			++numRedundantDirty_;
			return;
		}
	}

	dirtyEvent_.raise();
}

async::result<void> MetadataCache::flushDirty_() {
	// Blocks dirtied while a batch is being synchronized are picked up by the next iteration.
	uint64_t seenSeq = 0;
	std::vector<uint64_t> batch;
	while(true) {
		co_await dirtyEvent_.async_wait(seenSeq);

		protocols::ostrace::Timer timer;
		uint64_t numRedundant;
		{
			std::lock_guard dirtyLock{dirtyMutex_};

			// Take the sequence together with the batch, so that no request is missed.
			// TODO: Use async::sequenced_event::current_sequence() once it exists.
			seenSeq = dirtyEvent_.next_sequence() - 1;
			batch.assign(dirtyBlocks_.begin(), dirtyBlocks_.end());
			dirtyBlocks_.clear();
			numRedundant = numRedundantDirty_;
			numRedundantDirty_ = 0;
		}
		auto numBlocks = batch.size();

		auto frameSize = size_t{1} << blockPagesShift_;
		protocols::ostrace::Timer cleanTimer;
		for(auto block : batch) {
			auto synchronize = co_await helix_ng::synchronizeSpace(
					helix::BorrowedDescriptor{kHelNullHandle},
					blockAddress_(block), frameSize);
			HEL_CHECK(synchronize.error());
		}
		auto cleanTime = cleanTimer.elapsed();

		ostContext.emit(
			ostEvtMetadataClean,
			ostAttrTime(timer.elapsed()),
			ostAttrNumBlocks(numBlocks),
			ostAttrNumRedundant(numRedundant),
			ostAttrTimeCleanPages(cleanTime)
		);
	}
}

} // namespace blockfs
