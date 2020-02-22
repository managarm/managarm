
#include <type_traits>
#include "execution/coroutine.hpp"
#include "kernel.hpp"
#include "fiber.hpp"
#include "service_helpers.hpp"
#include <frg/container_of.hpp>
#include "types.hpp"

namespace thor {

extern size_t kernelMemoryUsage;

namespace {
	constexpr bool logCleanup = false;
	constexpr bool logUsage = false;

	// Perform more rigorous checks on spurious page faults.
	// Those checks should not be necessary if the code is correct but they help to catch bugs.
	constexpr bool thoroughSpuriousAssertions = true;

	constexpr bool disableCow = false;

	void logRss(AddressSpace *space) {
		if(!logUsage)
			return;
		auto rss = space->rss();
		if(!rss)
			return;
		auto b = 63 -__builtin_clz(rss);
		if(b < 1)
			return;
		if(rss & ((1 << (b - 1)) - 1))
			return;
		frigg::infoLogger() << "thor: RSS of " << space << " increases above "
				<< (rss / 1024) << " KiB" << frigg::endLog;
		frigg::infoLogger() << "thor:     Physical usage: "
				<< (physicalAllocator->numUsedPages() * 4) << " KiB, kernel usage: "
				<< (kernelMemoryUsage / 1024) << " KiB" << frigg::endLog;
	}
}

MemorySlice::MemorySlice(frigg::SharedPtr<MemoryView> view,
		ptrdiff_t view_offset, size_t view_size)
: _view{std::move(view)}, _viewOffset{view_offset}, _viewSize{view_size} {
	assert(!(_viewOffset & (kPageSize - 1)));
	assert(!(_viewSize & (kPageSize - 1)));
}

// --------------------------------------------------------
// HoleAggregator
// --------------------------------------------------------

bool HoleAggregator::aggregate(Hole *hole) {
	size_t size = hole->length();
	if(HoleTree::get_left(hole) && HoleTree::get_left(hole)->largestHole > size)
		size = HoleTree::get_left(hole)->largestHole;
	if(HoleTree::get_right(hole) && HoleTree::get_right(hole)->largestHole > size)
		size = HoleTree::get_right(hole)->largestHole;

	if(hole->largestHole == size)
		return false;
	hole->largestHole = size;
	return true;
}

bool HoleAggregator::check_invariant(HoleTree &tree, Hole *hole) {
	auto pred = tree.predecessor(hole);
	auto succ = tree.successor(hole);

	// Check largest hole invariant.
	size_t size = hole->length();
	if(tree.get_left(hole) && tree.get_left(hole)->largestHole > size)
		size = tree.get_left(hole)->largestHole;
	if(tree.get_right(hole) && tree.get_right(hole)->largestHole > size)
		size = tree.get_right(hole)->largestHole;

	if(hole->largestHole != size) {
		frigg::infoLogger() << "largestHole violation: " << "Expected " << size
				<< ", got " << hole->largestHole << "." << frigg::endLog;
		return false;
	}

	// Check non-overlapping memory areas invariant.
	if(pred && hole->address() < pred->address() + pred->length()) {
		frigg::infoLogger() << "Non-overlapping (left) violation" << frigg::endLog;
		return false;
	}
	if(succ && hole->address() + hole->length() > succ->address()) {
		frigg::infoLogger() << "Non-overlapping (right) violation" << frigg::endLog;
		return false;
	}

	return true;
}

// --------------------------------------------------------
// Mapping
// --------------------------------------------------------

Mapping::Mapping(size_t length, MappingFlags flags)
: _length{length}, _flags{flags} { }

void Mapping::tie(smarter::shared_ptr<AddressSpace> owner, VirtualAddr address) {
	assert(!_owner);
	assert(owner);
	_owner = std::move(owner);
	_address = address;
}

void Mapping::protect(MappingFlags flags) {
	std::underlying_type_t<MappingFlags> newFlags = _flags;
	newFlags &= ~(MappingFlags::protRead | MappingFlags::protWrite | MappingFlags::protExecute);
	newFlags |= flags;
	_flags = static_cast<MappingFlags>(newFlags);
}

bool Mapping::populateVirtualRange(PopulateVirtualNode *continuation) {
	execution::detach([] (Mapping *self, PopulateVirtualNode *continuation) -> coroutine<void> {
		size_t progress = 0;
		while(progress < continuation->_size) {
			auto [error, range, spurious] = co_await self->touchVirtualPage(continuation->_offset
					+ progress);
			assert(!error);
			progress += range.get<1>();
		}
		WorkQueue::post(continuation->_prepared);
	}(this, continuation));
	return false;
}

uint32_t Mapping::compilePageFlags() {
	uint32_t page_flags = 0;
	// TODO: Allow inaccessible mappings.
	assert(flags() & MappingFlags::protRead);
	if(flags() & MappingFlags::protWrite)
		page_flags |= page_access::write;
	if(flags() & MappingFlags::protExecute)
		page_flags |= page_access::execute;
	return page_flags;
}

// --------------------------------------------------------
// NormalMapping
// --------------------------------------------------------

NormalMapping::NormalMapping(size_t length, MappingFlags flags,
		frigg::SharedPtr<MemorySlice> slice, uintptr_t view_offset)
: Mapping{length, flags},
		_slice{std::move(slice)}, _viewOffset{view_offset} {
	assert(_viewOffset >= _slice->offset());
	assert(_viewOffset + NormalMapping::length() <= _slice->offset() + _slice->length());
	_view = _slice->getView();
}

NormalMapping::~NormalMapping() {
	assert(_state == MappingState::retired);
	//frigg::infoLogger() << "\e[31mthor: NormalMapping is destructed\e[39m" << frigg::endLog;
}

bool NormalMapping::lockVirtualRange(LockVirtualNode *node) {
	if(auto e = _view->lockRange(_viewOffset + node->offset(), node->size()); e)
		assert(!"lockRange() failed");
	return true;
}

void NormalMapping::unlockVirtualRange(uintptr_t offset, size_t size) {
	_view->unlockRange(_viewOffset + offset, size);
}

frg::tuple<PhysicalAddr, CachingMode>
NormalMapping::resolveRange(ptrdiff_t offset) {
	assert(_state == MappingState::active);

	// TODO: This function should be rewritten.
	assert((size_t)offset + kPageSize <= length());
	auto bundle_range = _view->peekRange(_viewOffset + offset);
	return frg::tuple<PhysicalAddr, CachingMode>{bundle_range.get<0>(), bundle_range.get<1>()};
}

bool NormalMapping::touchVirtualPage(TouchVirtualNode *continuation) {
	assert(_state == MappingState::active);

	struct Closure {
		NormalMapping *self;
		FetchNode fetch;
		Worklet worklet;
		TouchVirtualNode *continuation;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	struct Ops {
		static bool doFetch(Closure *closure) {
			auto self = closure->self;

			FetchFlags fetch_flags = 0;
			if(self->flags() & MappingFlags::dontRequireBacking)
				fetch_flags |= FetchNode::disallowBacking;

			if(auto e = self->_view->lockRange((self->_viewOffset + closure->continuation->_offset)
					& ~(kPageSize - 1), kPageSize); e)
				assert(!"lockRange() failed");

			closure->fetch.setup(&closure->worklet, fetch_flags);
			closure->worklet.setup([] (Worklet *base) {
				auto closure = frg::container_of(base, &Closure::worklet);
				auto self = closure->self;
				assert(!closure->fetch.error());
				mapPage(closure);
				self->_view->unlockRange((self->_viewOffset + closure->continuation->_offset)
						& ~(kPageSize - 1), kPageSize);
				closure->continuation->setResult(kErrSuccess, closure->fetch.range());

				// Tail of asynchronous path.
				WorkQueue::post(closure->continuation->_worklet);
				frigg::destruct(*kernelAlloc, closure);
			});
			if(!self->_view->fetchRange(self->_viewOffset + closure->continuation->_offset,
					&closure->fetch))
				return false;

			if(closure->fetch.error()) {
				closure->continuation->setResult(closure->fetch.error());
				return true;
			}
			mapPage(closure);
			self->_view->unlockRange((self->_viewOffset + closure->continuation->_offset)
					& ~(kPageSize - 1), kPageSize);
			closure->continuation->setResult(kErrSuccess, closure->fetch.range());
			return true;
		}

		static void mapPage(Closure *closure) {
			auto self = closure->self;
			auto page_offset = self->address() + closure->continuation->_offset;

			// TODO: Update RSS, handle dirty pages, etc.
			self->owner()->_pageSpace.unmapSingle4k(page_offset & ~(kPageSize - 1));
			self->owner()->_pageSpace.mapSingle4k(page_offset & ~(kPageSize - 1),
					closure->fetch.range().get<0>() & ~(kPageSize - 1),
					true, self->compilePageFlags(), closure->fetch.range().get<2>());
			self->owner()->_residuentSize += kPageSize;
			logRss(self->owner());
		}
	};

	closure->self = this;
	closure->continuation = continuation;

	if(!Ops::doFetch(closure))
		return false;

	frigg::destruct(*kernelAlloc, closure);
	return true;
}

smarter::shared_ptr<Mapping> NormalMapping::forkMapping() {
	auto mapping = smarter::allocate_shared<NormalMapping>(Allocator{},
			length(), flags(), _slice, _viewOffset);
	mapping->selfPtr = mapping;
	return std::move(mapping);
}

void NormalMapping::install() {
	assert(_state == MappingState::null);
	_state = MappingState::active;
	_view->addObserver(smarter::static_pointer_cast<NormalMapping>(selfPtr.lock()));

	uint32_t page_flags = 0;
	if((flags() & MappingFlags::permissionMask) & MappingFlags::protWrite)
		page_flags |= page_access::write;
	if((flags() & MappingFlags::permissionMask) & MappingFlags::protExecute)
		page_flags |= page_access::execute;
	// TODO: Allow inaccessible mappings.
	assert((flags() & MappingFlags::permissionMask) & MappingFlags::protRead);

	if(auto e = _view->lockRange(_viewOffset, length()); e)
		assert(!"lockRange() failed");

	for(size_t progress = 0; progress < length(); progress += kPageSize) {
		auto bundle_range = _view->peekRange(_viewOffset + progress);

		VirtualAddr vaddr = address() + progress;
		assert(!owner()->_pageSpace.isMapped(vaddr));

		if(bundle_range.get<0>() != PhysicalAddr(-1)) {
			owner()->_pageSpace.mapSingle4k(vaddr, bundle_range.get<0>(), true,
					page_flags, bundle_range.get<1>());
			owner()->_residuentSize += kPageSize;
			logRss(owner());
		}
	}

	_view->unlockRange(_viewOffset, length());
}

void NormalMapping::reinstall() {
	// TODO: Implement this properly.
	//       Note that we need to call markDirty() on dirty pages that we unmap here.
	assert(!"reinstall() is not implemented for NormalMapping");
}

void NormalMapping::uninstall() {
	assert(_state == MappingState::active);
	_state = MappingState::zombie;

	for(size_t pg = 0; pg < length(); pg += kPageSize) {
		auto status = owner()->_pageSpace.unmapSingle4k(address() + pg);
		if(!(status & page_status::present))
			continue;
		if(status & page_status::dirty)
			_view->markDirty(_viewOffset + pg, kPageSize);
		owner()->_residuentSize -= kPageSize;
	}
}

void NormalMapping::retire() {
	assert(_state == MappingState::zombie);
	_view->removeObserver(smarter::static_pointer_cast<NormalMapping>(selfPtr));
	_state = MappingState::retired;
}

bool NormalMapping::observeEviction(uintptr_t evict_offset, size_t evict_length,
		EvictNode *continuation) {
	assert(_state == MappingState::active);

	if(evict_offset + evict_length <= _viewOffset
			|| evict_offset >= _viewOffset + length())
		return true;

	// Begin and end offsets of the region that we need to unmap.
	auto shoot_begin = frg::max(evict_offset, _viewOffset);
	auto shoot_end = frg::min(evict_offset + evict_length, _viewOffset + length());

	// Offset from the beginning of the mapping.
	auto shoot_offset = shoot_begin - _viewOffset;
	auto shoot_size = shoot_end - shoot_begin;
	assert(shoot_size);
	assert(!(shoot_offset & (kPageSize - 1)));
	assert(!(shoot_size & (kPageSize - 1)));

	// TODO: Perform proper locking here!

	// Unmap the memory range.
	for(size_t pg = 0; pg < shoot_size; pg += kPageSize) {
		auto status = owner()->_pageSpace.unmapSingle4k(address() + shoot_offset + pg);
		if(!(status & page_status::present))
			continue;
		if(status & page_status::dirty)
			_view->markDirty(_viewOffset + shoot_offset + pg, kPageSize);
		owner()->_residuentSize -= kPageSize;
	}

	// Perform shootdown.
	struct Closure {
		smarter::shared_ptr<Mapping> mapping; // Need to keep the Mapping alive.
		Worklet worklet;
		ShootNode node;
		EvictNode *continuation;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	closure->worklet.setup([] (Worklet *base) {
		auto closure = frg::container_of(base, &Closure::worklet);
		closure->continuation->done();
		frigg::destruct(*kernelAlloc, closure);
	});
	closure->mapping = selfPtr.lock();
	closure->continuation = continuation;

	closure->node.address = address() + shoot_offset;
	closure->node.size = shoot_size;
	closure->node.setup(&closure->worklet);
	if(!owner()->_pageSpace.submitShootdown(&closure->node))
		return false;

	frigg::destruct(*kernelAlloc, closure);
	return true;
}

// --------------------------------------------------------
// CowMapping
// --------------------------------------------------------

CowChain::CowChain(frigg::SharedPtr<CowChain> chain)
: _superChain{std::move(chain)}, _pages{*kernelAlloc} {
}

CowChain::~CowChain() {
	if(logCleanup)
		frigg::infoLogger() << "thor: Releasing CowChain" << frigg::endLog;

	for(auto it = _pages.begin(); it != _pages.end(); ++it) {
		auto physical = it->load(std::memory_order_relaxed);
		assert(physical != PhysicalAddr(-1));
		physicalAllocator->free(physical, kPageSize);
	}
}

// --------------------------------------------------------

CowMapping::CowMapping(size_t length, MappingFlags flags,
		frigg::SharedPtr<MemorySlice> slice, uintptr_t view_offset,
		frigg::SharedPtr<CowChain> chain)
: Mapping{length, flags},
		_slice{std::move(slice)}, _viewOffset{view_offset}, _copyChain{std::move(chain)},
		_ownedPages{*kernelAlloc} {
	assert(!(length & (kPageSize - 1)));
	assert(!(_viewOffset & (kPageSize - 1)));
}

CowMapping::~CowMapping() {
	assert(_state == MappingState::retired);
	if(logCleanup)
		frigg::infoLogger() << "\e[31mthor: CowMapping is destructed\e[39m" << frigg::endLog;

	for(auto it = _ownedPages.begin(); it != _ownedPages.end(); ++it) {
		assert(it->state == CowState::hasCopy);
		assert(it->physical != PhysicalAddr(-1));
		physicalAllocator->free(it->physical, kPageSize);
	}
}

bool CowMapping::lockVirtualRange(LockVirtualNode *continuation) {
	// For now, it is enough to populate the range, as pages can only be evicted from
	// the root of the CoW chain, but copies are never evicted.

	struct Closure {
		CowMapping *self;
		size_t progress = 0;
		PhysicalAddr physical;
		PageAccessor accessor;
		CopyFromBundleNode copy;
		ShootNode shoot;
		Worklet worklet;
		LockVirtualNode *continuation;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	struct Ops {
		static bool process(Closure *closure) {
			while(closure->progress < closure->continuation->size())
				if(!copyPage(closure))
					return false;
			return true;
		}

		static bool copyPage(Closure *closure) {
			auto self = closure->self;
			auto offset = closure->continuation->offset() + closure->progress;

			frigg::SharedPtr<CowChain> chain;
			frigg::SharedPtr<MemoryView> view;
			uintptr_t view_offset;
			{
				// If the page is present in our private chain, we just return it.
				auto irq_lock = frigg::guard(&irqMutex());
				auto lock = frigg::guard(&self->_mutex);
				assert(self->_state == MappingState::active);

				if(auto it = self->_ownedPages.find(offset >> kPageShift);
						it) {
					assert(it->state == CowState::hasCopy);
					assert(it->physical != PhysicalAddr(-1));

					it->lockCount++;
					closure->progress += kPageSize;
					return true;
				}

				chain = self->_copyChain;
				view = self->_slice->getView();
				view_offset = self->_viewOffset;

				// Otherwise we need to copy from the chain or from the root view.
				auto it = self->_ownedPages.insert(offset >> kPageShift);
				it->state = CowState::inProgress;
			}

			closure->physical = physicalAllocator->allocate(kPageSize);
			assert(closure->physical != PhysicalAddr(-1) && "OOM");
			closure->accessor = PageAccessor{closure->physical};

			// Try to copy from a descendant CoW chain.
			auto page_offset = view_offset + offset;
			while(chain) {
				auto irq_lock = frigg::guard(&irqMutex());
				auto lock = frigg::guard(&chain->_mutex);

				if(auto it = chain->_pages.find(page_offset >> kPageShift); it) {
					// We can just copy synchronously here -- the descendant is not evicted.
					auto src_physical = it->load(std::memory_order_relaxed);
					assert(src_physical != PhysicalAddr(-1));
					auto src_accessor = PageAccessor{src_physical};
					memcpy(closure->accessor.get(), src_accessor.get(), kPageSize);
					break;
				}

				chain = chain->_superChain;
			}

			if(chain) {
				auto irq_lock = frigg::guard(&irqMutex());
				auto lock = frigg::guard(&self->_mutex);

				return mapPage(closure);
			}

			// Copy from the root view.
			closure->worklet.setup([] (Worklet *base) {
				auto closure = frg::container_of(base, &Closure::worklet);
				{
					auto irq_lock = frigg::guard(&irqMutex());
					auto lock = frigg::guard(&closure->self->_mutex);

					if(!mapPage(closure))
						return;
				}

				// Tail of asynchronous path.
				if(!process(closure))
					return;
				LockVirtualNode::post(closure->continuation);
				frigg::destruct(*kernelAlloc, closure);
			});
			auto complete = [] (CopyFromBundleNode *base) {
				// As CopyFromBundle calls its argument inline, we have to
				// do this annoying detour of posting the worklet here.
				auto closure = frg::container_of(base, &Closure::copy);
				WorkQueue::post(&closure->worklet);
			};
			if(!copyFromBundle(view.get(), page_offset & ~(kPageSize - 1),
					closure->accessor.get(), kPageSize,
					&closure->copy, complete))
				return false;

			auto irq_lock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&self->_mutex);

			return mapPage(closure);
		}

		static void insertPage(Closure *closure) {
			// TODO: Assert that there is no overflow.
			auto self = closure->self;
			auto offset = closure->continuation->offset() + closure->progress;
			auto address = self->address() + offset;

			// Remap the copy as read-write.
			auto status = self->owner()->_pageSpace.unmapSingle4k(address & ~(kPageSize - 1));
			assert(!(status & page_status::dirty));
			self->owner()->_pageSpace.mapSingle4k(address & ~(kPageSize - 1),
					closure->physical,
					true, self->compilePageFlags(), CachingMode::null);

			auto cow_it = self->_ownedPages.find(offset >> kPageShift);
			assert(cow_it->state == CowState::inProgress);
			cow_it->state = CowState::hasCopy;
			cow_it->physical = closure->physical;
			cow_it->lockCount++;
			closure->progress += kPageSize;
		}

		static bool mapPage(Closure *closure) {
			auto self = closure->self;
			auto address = self->address()
					+ closure->continuation->offset() + closure->progress;

			// To make CoW unobservable, we first need to map the copy as read-only,
			// perform shootdown and finally map the copy as read-write.
			// This guarantees that all observers see the copy before the first write is done.

			// TODO: Update RSS, handle dirty pages, etc.
			// The page is a copy with default caching mode.
			auto status = self->owner()->_pageSpace.unmapSingle4k(address & ~(kPageSize - 1));
			assert(!(status & page_status::dirty));
			self->owner()->_pageSpace.mapSingle4k(address & ~(kPageSize - 1),
					closure->physical,
					true, self->compilePageFlags() & ~page_access::write, CachingMode::null);
			self->owner()->_residuentSize += kPageSize;
			logRss(self->owner());

			closure->worklet.setup([] (Worklet *base) {
				auto closure = frg::container_of(base, &Closure::worklet);
				{
					auto irq_lock = frigg::guard(&irqMutex());
					auto lock = frigg::guard(&closure->self->_mutex);

					insertPage(closure);
				}

				// Tail of asynchronous path.
				if(!process(closure))
					return;
				LockVirtualNode::post(closure->continuation);
				frigg::destruct(*kernelAlloc, closure);
			});
			closure->shoot.address = address & ~(kPageSize - 1);
			closure->shoot.size = kPageSize;
			closure->shoot.setup(&closure->worklet);
			if(!self->owner()->_pageSpace.submitShootdown(&closure->shoot))
				return false;

			insertPage(closure);
			return true;
		}
	};

	closure->self = this;
	closure->continuation = continuation;

	if(!Ops::process(closure))
		return false;

	frigg::destruct(*kernelAlloc, closure);
	return true;
}

void CowMapping::unlockVirtualRange(uintptr_t offset, size_t size) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);
	assert(_state == MappingState::active);

	for(size_t pg = 0; pg < size; pg += kPageSize) {
		auto it = _ownedPages.find((offset + pg) >> kPageShift);
		assert(it);
		assert(it->state == CowState::hasCopy);
		assert(it->lockCount > 0);
		it->lockCount--;
	}
}

frg::tuple<PhysicalAddr, CachingMode>
CowMapping::resolveRange(ptrdiff_t offset) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);
	assert(_state == MappingState::active);

	if(auto it = _ownedPages.find(offset >> kPageShift); it) {
		assert(it->state == CowState::hasCopy);
		return frg::tuple<PhysicalAddr, CachingMode>{it->physical, CachingMode::null};
	}

	return frg::tuple<PhysicalAddr, CachingMode>{PhysicalAddr(-1), CachingMode::null};
}

bool CowMapping::touchVirtualPage(TouchVirtualNode *continuation) {
	struct Closure {
		CowMapping *self;
		PhysicalAddr physical;
		PageAccessor accessor;
		CopyFromBundleNode copy;
		ShootNode shoot;
		Worklet worklet;
		TouchVirtualNode *continuation;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	struct Ops {
		static bool copyPage(Closure *closure) {
			auto self = closure->self;
			auto misalign = closure->continuation->_offset & (kPageSize - 1);

			frigg::SharedPtr<CowChain> chain;
			frigg::SharedPtr<MemoryView> view;
			uintptr_t view_offset;
			{
				// If the page is present in our private chain, we just return it.
				auto irq_lock = frigg::guard(&irqMutex());
				auto lock = frigg::guard(&self->_mutex);
				assert(self->_state == MappingState::active);

				if(auto it = self->_ownedPages.find(closure->continuation->_offset >> kPageShift);
						it) {
					assert(it->state == CowState::hasCopy);
					if(thoroughSpuriousAssertions) {
						ClientPageSpace::Walk walk{&self->owner()->_pageSpace};
						walk.walkTo(self->address() + closure->continuation->_offset);
						assert(it->physical == walk.peekPhysical());
						assert(walk.peekFlags() & page_access::write);
					}

					closure->continuation->setResult(kErrSuccess, it->physical + misalign,
							kPageSize - misalign, CachingMode::null, true);
					return true;
				}

				chain = self->_copyChain;
				view = self->_slice->getView();
				view_offset = self->_viewOffset;

				// Otherwise we need to copy from the chain or from the root view.
				auto it = self->_ownedPages.insert(closure->continuation->_offset >> kPageShift);
				it->state = CowState::inProgress;
			}

			closure->physical = physicalAllocator->allocate(kPageSize);
			assert(closure->physical != PhysicalAddr(-1) && "OOM");
			closure->accessor = PageAccessor{closure->physical};

			// Try to copy from a descendant CoW chain.
			auto page_offset = view_offset + closure->continuation->_offset;
			while(chain) {
				auto irq_lock = frigg::guard(&irqMutex());
				auto lock = frigg::guard(&chain->_mutex);

				if(auto it = chain->_pages.find(page_offset >> kPageShift); it) {
					// We can just copy synchronously here -- the descendant is not evicted.
					auto src_physical = it->load(std::memory_order_relaxed);
					assert(src_physical != PhysicalAddr(-1));
					auto src_accessor = PageAccessor{src_physical};
					memcpy(closure->accessor.get(), src_accessor.get(), kPageSize);
					break;
				}

				chain = chain->_superChain;
			}

			if(chain) {
				auto irq_lock = frigg::guard(&irqMutex());
				auto lock = frigg::guard(&self->_mutex);

				return mapPage(closure);
			}

			// Copy from the root view.
			closure->worklet.setup([] (Worklet *base) {
				auto closure = frg::container_of(base, &Closure::worklet);
				{
					auto irq_lock = frigg::guard(&irqMutex());
					auto lock = frigg::guard(&closure->self->_mutex);

					if(!mapPage(closure))
						return;
				}

				// Tail of asynchronous path.
				WorkQueue::post(closure->continuation->_worklet);
				frigg::destruct(*kernelAlloc, closure);
			});
			auto complete = [] (CopyFromBundleNode *base) {
				// As CopyFromBundle calls its argument inline, we have to
				// do this annoying detour of posting the worklet here.
				auto closure = frg::container_of(base, &Closure::copy);
				WorkQueue::post(&closure->worklet);
			};
			if(!copyFromBundle(view.get(), page_offset & ~(kPageSize - 1),
					closure->accessor.get(), kPageSize,
					&closure->copy, complete))
				return false;

			auto irq_lock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&self->_mutex);

			return mapPage(closure);
		}

		static void insertPage(Closure *closure) {
			// TODO: Assert that there is no overflow.
			auto self = closure->self;
			auto address = self->address() + closure->continuation->_offset;
			auto misalign = closure->continuation->_offset & (kPageSize - 1);

			// Remap the copy as read-write.
			auto status = self->owner()->_pageSpace.unmapSingle4k(address & ~(kPageSize - 1));
			assert(!(status & page_status::dirty));
			self->owner()->_pageSpace.mapSingle4k(address & ~(kPageSize - 1),
					closure->physical,
					true, self->compilePageFlags(), CachingMode::null);

			closure->continuation->setResult(kErrSuccess, closure->physical + misalign,
					kPageSize - misalign, CachingMode::null);
			auto cow_it = self->_ownedPages.find(closure->continuation->_offset >> kPageShift);
			assert(cow_it->state == CowState::inProgress);
			cow_it->state = CowState::hasCopy;
			cow_it->physical = closure->physical;
		}

		static bool mapPage(Closure *closure) {
			auto self = closure->self;
			auto address = self->address() + closure->continuation->_offset;

			// To make CoW unobservable, we first need to map the copy as read-only,
			// perform shootdown and finally map the copy as read-write.
			// This guarantees that all observers see the copy before the first write is done.

			// TODO: Update RSS, handle dirty pages, etc.
			// The page is a copy with default caching mode.
			auto status = self->owner()->_pageSpace.unmapSingle4k(address & ~(kPageSize - 1));
			assert(!(status & page_status::dirty));
			self->owner()->_pageSpace.mapSingle4k(address & ~(kPageSize - 1),
					closure->physical,
					true, self->compilePageFlags() & ~page_access::write, CachingMode::null);
			self->owner()->_residuentSize += kPageSize;
			logRss(self->owner());

			closure->worklet.setup([] (Worklet *base) {
				auto closure = frg::container_of(base, &Closure::worklet);
				{
					auto irq_lock = frigg::guard(&irqMutex());
					auto lock = frigg::guard(&closure->self->_mutex);

					insertPage(closure);
				}

				// Tail of asynchronous path.
				WorkQueue::post(closure->continuation->_worklet);
				frigg::destruct(*kernelAlloc, closure);
			});
			closure->shoot.address = address & ~(kPageSize - 1);
			closure->shoot.size = kPageSize;
			closure->shoot.setup(&closure->worklet);
			if(!self->owner()->_pageSpace.submitShootdown(&closure->shoot))
				return false;

			insertPage(closure);
			return true;
		}
	};

	closure->self = this;
	closure->continuation = continuation;

	if(!Ops::copyPage(closure))
		return false;

	frigg::destruct(*kernelAlloc, closure);
	return true;
}

smarter::shared_ptr<Mapping> CowMapping::forkMapping() {
	// Note that locked pages require special attention during CoW: as we cannot
	// replace them by copies, we have to copy them eagerly.
	// Therefore, they are special-cased below.

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	// Create a new CowChain for both the original and the forked mapping.
	// To correct handle locks pages, we move only non-locked pages from
	// the original mapping to the new chain.
	auto new_chain = frigg::makeShared<CowChain>(*kernelAlloc, _copyChain);

	// Update the original mapping
	_copyChain = new_chain;

	// Create a new mapping in the forked space.
	auto forked = smarter::allocate_shared<CowMapping>(Allocator{},
			length(), flags(),
			_slice, _viewOffset, new_chain);
	forked->selfPtr = forked;

	// Finally, inspect all copied pages owned by the original mapping.
	for(size_t pg = 0; pg < length(); pg += kPageSize) {
		auto os_it = _ownedPages.find(pg >> kPageShift);

		if(!os_it)
			continue;
		assert(os_it->state == CowState::hasCopy);

		// The page is locked. We *need* to keep it in the old address space.
		if(os_it->lockCount || disableCow) {
			// Allocate a new physical page for a copy.
			auto copy_physical = physicalAllocator->allocate(kPageSize);
			assert(copy_physical != PhysicalAddr(-1) && "OOM");

			// As the page is locked anyway, we can just copy it synchronously.
			PageAccessor locked_accessor{os_it->physical};
			PageAccessor copy_accessor{copy_physical};
			memcpy(copy_accessor.get(), locked_accessor.get(), kPageSize);

			// Update the chains.
			auto fs_it = forked->_ownedPages.insert(pg >> kPageShift);
			fs_it->state = CowState::hasCopy;
			fs_it->physical = copy_physical;
		}else{
			auto physical = os_it->physical;
			assert(physical != PhysicalAddr(-1));

			// Update the chains.
			auto page_offset = _viewOffset + pg;
			auto new_it = new_chain->_pages.insert(page_offset >> kPageShift,
					PhysicalAddr(-1));
			_ownedPages.erase(pg >> kPageShift);
			new_it->store(physical, std::memory_order_relaxed);

			// TODO: Increment _residentSize, handle dirty pages, etc.
			owner()->_pageSpace.unmapSingle4k(address() + pg);

			// Keep the page mapped as read-only.
			// As the page comes from a CowChain, it has a default caching mode.
			owner()->_pageSpace.mapSingle4k(address() + pg, physical, true,
					compilePageFlags() & ~page_access::write, CachingMode::null);
		}
	}

	return forked;
}

void CowMapping::install() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);
	assert(_state == MappingState::null);

	_state = MappingState::active;

	_slice->getView()->addObserver(
			smarter::static_pointer_cast<CowMapping>(selfPtr.lock()));

	auto findBorrowedPage = [&] (uintptr_t offset) -> frg::tuple<PhysicalAddr, CachingMode> {
		auto page_offset = _viewOffset + offset;

		// Get the page from a descendant CoW chain.
		auto chain = _copyChain;
		while(chain) {
			auto irq_lock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&chain->_mutex);

			if(auto it = chain->_pages.find(page_offset >> kPageShift); it) {
				// We can just copy synchronously here -- the descendant is not evicted.
				auto physical = it->load(std::memory_order_relaxed);
				assert(physical != PhysicalAddr(-1));
				return frg::tuple<PhysicalAddr, CachingMode>{physical, CachingMode::null};
			}

			chain = chain->_superChain;
		}

		// Get the page from the root view.
		return _slice->getView()->peekRange(page_offset);
	};

	if(auto e = _slice->getView()->lockRange(_viewOffset, length()); e)
		assert(!"lockRange() failed");

	for(size_t pg = 0; pg < length(); pg += kPageSize) {
		if(auto it = _ownedPages.find(pg >> kPageShift); it) {
			// TODO: Update RSS.
			assert(it->state == CowState::hasCopy);
			assert(it->physical != PhysicalAddr(-1));
			owner()->_pageSpace.mapSingle4k(address() + pg,
					it->physical, true, compilePageFlags(), CachingMode::null);
		}else if(auto range = findBorrowedPage(pg); range.get<0>() != PhysicalAddr(-1)) {
			// Note that we have to mask the writeable flag here.
			// TODO: Update RSS.
			owner()->_pageSpace.mapSingle4k(address() + pg, range.get<0>(), true,
					compilePageFlags() & ~page_access::write, range.get<1>());
		}
	}

	_slice->getView()->unlockRange(_viewOffset, length());
}

void CowMapping::reinstall() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);
	assert(_state == MappingState::active);

	auto findBorrowedPage = [&] (uintptr_t offset) -> frg::tuple<PhysicalAddr, CachingMode> {
		auto page_offset = _viewOffset + offset;

		// Get the page from a descendant CoW chain.
		auto chain = _copyChain;
		while(chain) {
			auto irq_lock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&chain->_mutex);

			if(auto it = chain->_pages.find(page_offset >> kPageShift); it) {
				// We can just copy synchronously here -- the descendant is not evicted.
				auto physical = it->load(std::memory_order_relaxed);
				assert(physical != PhysicalAddr(-1));
				return frg::tuple<PhysicalAddr, CachingMode>{physical, CachingMode::null};
			}

			chain = chain->_superChain;
		}

		// Get the page from the root view.
		return _slice->getView()->peekRange(page_offset);
	};

	if(auto e = _slice->getView()->lockRange(_viewOffset, length()); e)
		assert(!"lockRange() failed");

	for(size_t pg = 0; pg < length(); pg += kPageSize) {
		if(auto it = _ownedPages.find(pg >> kPageShift); it) {
			// TODO: Update RSS.
			assert(it->state == CowState::hasCopy);
			assert(it->physical != PhysicalAddr(-1));
			owner()->_pageSpace.unmapSingle4k(address() + pg);
			owner()->_pageSpace.mapSingle4k(address() + pg,
					it->physical, true, compilePageFlags(), CachingMode::null);
		}else if(auto range = findBorrowedPage(pg); range.get<0>() != PhysicalAddr(-1)) {
			// Note that we have to mask the writeable flag here.
			// TODO: Update RSS.
			owner()->_pageSpace.unmapSingle4k(address() + pg);
			owner()->_pageSpace.mapSingle4k(address() + pg, range.get<0>(), true,
					compilePageFlags() & ~page_access::write, range.get<1>());
		}else{
			assert(!owner()->_pageSpace.isMapped(address() + pg));
		}
	}

	_slice->getView()->unlockRange(_viewOffset, length());
}

void CowMapping::uninstall() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);
	assert(_state == MappingState::active);

	_state = MappingState::zombie;

	for(size_t pg = 0; pg < length(); pg += kPageSize)
		if(owner()->_pageSpace.isMapped(address() + pg))
			owner()->_residuentSize -= kPageSize;
	owner()->_pageSpace.unmapRange(address(), length(), PageMode::remap);
}

void CowMapping::retire() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);
	assert(_state == MappingState::zombie);

	_slice->getView()->removeObserver(
			smarter::static_pointer_cast<CowMapping>(selfPtr));
	_state = MappingState::retired;
}

bool CowMapping::observeEviction(uintptr_t evict_offset, size_t evict_length,
		EvictNode *continuation) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);
	assert(_state == MappingState::active);

	if(evict_offset + evict_length <= _viewOffset
			|| evict_offset >= _viewOffset + length())
		return true;

	// Begin and end offsets of the region that we need to unmap.
	auto shoot_begin = frg::max(evict_offset, _viewOffset);
	auto shoot_end = frg::min(evict_offset + evict_length, _viewOffset + length());

	// Offset from the beginning of the mapping.
	auto shoot_offset = shoot_begin - _viewOffset;
	auto shoot_size = shoot_end - shoot_begin;
	assert(shoot_size);
	assert(!(shoot_offset & (kPageSize - 1)));
	assert(!(shoot_size & (kPageSize - 1)));

	// TODO: Perform proper locking here!

	// Unmap the memory range.
	for(size_t pg = 0; pg < shoot_size; pg += kPageSize) {
		if(auto it = _ownedPages.find((shoot_offset + pg) >> kPageShift); it)
			continue;
		if(owner()->_pageSpace.isMapped(address() + shoot_offset + pg))
			owner()->_residuentSize -= kPageSize;
		auto status = owner()->_pageSpace.unmapSingle4k(address() + shoot_offset + pg);
		assert(!(status & page_status::dirty));
	}

	// Perform shootdown.
	struct Closure {
		smarter::shared_ptr<Mapping> mapping; // Need to keep the Mapping alive.
		Worklet worklet;
		ShootNode node;
		EvictNode *continuation;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	closure->worklet.setup([] (Worklet *base) {
		auto closure = frg::container_of(base, &Closure::worklet);
		closure->continuation->done();
		frigg::destruct(*kernelAlloc, closure);
	});
	closure->mapping = selfPtr.lock();
	closure->continuation = continuation;

	closure->node.address = address() + shoot_offset;
	closure->node.size = shoot_size;
	closure->node.setup(&closure->worklet);
	if(!owner()->_pageSpace.submitShootdown(&closure->node))
		return false;

	frigg::destruct(*kernelAlloc, closure);
	return true;
}

// --------------------------------------------------------
// AddressSpace
// --------------------------------------------------------

void AddressSpace::activate(smarter::shared_ptr<AddressSpace, BindableHandle> space) {
	auto page_space = &space->_pageSpace;
	PageSpace::activate(smarter::shared_ptr<PageSpace>{space->selfPtr.lock(), page_space});
}

AddressSpace::AddressSpace() { }

AddressSpace::~AddressSpace() {
	if(logCleanup)
		frigg::infoLogger() << "\e[31mthor: AddressSpace is destructed\e[39m" << frigg::endLog;

	while(_holes.get_root()) {
		auto hole = _holes.get_root();
		_holes.remove(hole);
		frg::destruct(*kernelAlloc, hole);
	}
}

void AddressSpace::dispose(BindableHandle) {
	if(logCleanup)
		frigg::infoLogger() << "\e[31mthor: AddressSpace is cleared\e[39m" << frigg::endLog;

	// TODO: Set some flag to make sure that no mappings are added/deleted.
	auto mapping = _mappings.first();
	while(mapping) {
		mapping->uninstall();
		mapping = MappingTree::successor(mapping);
	}

	struct Closure {
		smarter::shared_ptr<AddressSpace> self;
		PageSpace::RetireNode retireNode;
		Worklet worklet;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	closure->self = selfPtr.lock();

	closure->retireNode.setup(&closure->worklet);
	closure->worklet.setup([] (Worklet *base) {
		auto closure = frg::container_of(base, &Closure::worklet);
		auto self = closure->self.get();

		while(self->_mappings.get_root()) {
			auto mapping = self->_mappings.get_root();
			mapping->retire();
			self->_mappings.remove(mapping);
			mapping->selfPtr.ctr()->decrement();
		}

		frg::destruct(*kernelAlloc, closure);
	});
	_pageSpace.retire(&closure->retireNode);
}

void AddressSpace::setupDefaultMappings() {
	auto hole = frigg::construct<Hole>(*kernelAlloc, 0x100000, 0x7ffffff00000);
	_holes.insert(hole);
}

smarter::shared_ptr<Mapping> AddressSpace::getMapping(VirtualAddr address) {
	auto irq_lock = frigg::guard(&irqMutex());
	AddressSpace::Guard space_guard(&lock);

	return _findMapping(address);
}

Error AddressSpace::map(Guard &guard,
		frigg::UnsafePtr<MemorySlice> slice, VirtualAddr address,
		size_t offset, size_t length, uint32_t flags, VirtualAddr *actual_address) {
	assert(guard.protects(&lock));
	assert(length);
	assert(!(length % kPageSize));

	if(offset + length > slice->length())
		return kErrBufferTooSmall;

	VirtualAddr target;
	if(flags & kMapFixed) {
		assert(address);
		assert((address % kPageSize) == 0);
		target = _allocateAt(address, length);
	}else{
		target = _allocate(length, flags);
	}
	assert(target);

//	frigg::infoLogger() << "Creating new mapping at " << (void *)target
//			<< ", length: " << (void *)length << frigg::endLog;

	// Setup a new Mapping object.
	std::underlying_type_t<MappingFlags> mapping_flags = 0;

	// TODO: This is a hack to "upgrade" CoW@fork to CoW.
	// TODO: Remove this once we remove CopyOnWriteAtFork.
	if(flags & kMapCopyOnWriteAtFork)
		flags |= kMapCopyOnWrite;
	if((flags & kMapCopyOnWrite) || (flags & kMapCopyOnWriteAtFork))
		mapping_flags |= MappingFlags::copyOnWriteAtFork;

	if(flags & kMapDropAtFork) {
		mapping_flags |= MappingFlags::dropAtFork;
	}else if(flags & kMapShareAtFork) {
		mapping_flags |= MappingFlags::shareAtFork;
	}

	// TODO: The upgrading mechanism needs to be arch-specific:
	// Some archs might only support RX, while other support X.
	auto mask = kMapProtRead | kMapProtWrite | kMapProtExecute;
	if((flags & mask) == (kMapProtRead | kMapProtWrite | kMapProtExecute)
			|| (flags & mask) == (kMapProtWrite | kMapProtExecute)) {
		// WX is upgraded to RWX.
		mapping_flags |= MappingFlags::protRead | MappingFlags::protWrite
			| MappingFlags::protExecute;
	}else if((flags & mask) == (kMapProtRead | kMapProtExecute)
			|| (flags & mask) == kMapProtExecute) {
		// X is upgraded to RX.
		mapping_flags |= MappingFlags::protRead | MappingFlags::protExecute;
	}else if((flags & mask) == (kMapProtRead | kMapProtWrite)
			|| (flags & mask) == kMapProtWrite) {
		// W is upgraded to RW.
		mapping_flags |= MappingFlags::protRead | MappingFlags::protWrite;
	}else if((flags & mask) == kMapProtRead) {
		mapping_flags |= MappingFlags::protRead;
	}else{
		assert(!(flags & mask));
	}

	if(flags & kMapDontRequireBacking)
		mapping_flags |= MappingFlags::dontRequireBacking;

	smarter::shared_ptr<Mapping> mapping;
	if(flags & kMapCopyOnWrite) {
		mapping = smarter::allocate_shared<CowMapping>(Allocator{},
				length, static_cast<MappingFlags>(mapping_flags),
				slice.toShared(), slice->offset() + offset, nullptr);
		mapping->selfPtr = mapping;
	}else{
		assert(!(mapping_flags & MappingFlags::copyOnWriteAtFork));
		mapping = smarter::allocate_shared<NormalMapping>(Allocator{},
				length, static_cast<MappingFlags>(mapping_flags),
				slice.toShared(), slice->offset() + offset);
		mapping->selfPtr = mapping;
	}

	assert(!(flags & kMapPopulate));

	// Install the new mapping object.
	mapping->tie(selfPtr.lock(), target);
	_mappings.insert(mapping.get());
	mapping->install();
	mapping.release(); // AddressSpace owns one reference.

	*actual_address = target;
	return kErrSuccess;
}

bool AddressSpace::protect(VirtualAddr address, size_t length,
		uint32_t flags, AddressProtectNode *node) {
	std::underlying_type_t<MappingFlags> mappingFlags = 0;

	// TODO: The upgrading mechanism needs to be arch-specific:
	// Some archs might only support RX, while other support X.
	auto mask = kMapProtRead | kMapProtWrite | kMapProtExecute;
	if((flags & mask) == (kMapProtRead | kMapProtWrite | kMapProtExecute)
			|| (flags & mask) == (kMapProtWrite | kMapProtExecute)) {
		// WX is upgraded to RWX.
		mappingFlags |= MappingFlags::protRead | MappingFlags::protWrite
			| MappingFlags::protExecute;
	}else if((flags & mask) == (kMapProtRead | kMapProtExecute)
			|| (flags & mask) == kMapProtExecute) {
		// X is upgraded to RX.
		mappingFlags |= MappingFlags::protRead | MappingFlags::protExecute;
	}else if((flags & mask) == (kMapProtRead | kMapProtWrite)
			|| (flags & mask) == kMapProtWrite) {
		// W is upgraded to RW.
		mappingFlags |= MappingFlags::protRead | MappingFlags::protWrite;
	}else if((flags & mask) == kMapProtRead) {
		mappingFlags |= MappingFlags::protRead;
	}else{
		assert(!(flags & mask));
	}

	auto irq_lock = frigg::guard(&irqMutex());
	AddressSpace::Guard space_guard(&lock);

	auto mapping = _findMapping(address);
	assert(mapping);

	// TODO: Allow shrinking of the mapping.
	assert(mapping->address() == address);
	assert(mapping->length() == length);
	mapping->protect(static_cast<MappingFlags>(mappingFlags));
	mapping->reinstall();

	node->_worklet.setup([] (Worklet *base) {
		auto node = frg::container_of(base, &AddressUnmapNode::_worklet);
		node->complete();
	});

	node->_shootNode.address = address;
	node->_shootNode.size = length;
	node->_shootNode.setup(&node->_worklet);
	if(!_pageSpace.submitShootdown(&node->_shootNode))
		return false;
	return true;
}

bool AddressSpace::unmap(VirtualAddr address, size_t length, AddressUnmapNode *node) {
	auto irq_lock = frigg::guard(&irqMutex());
	AddressSpace::Guard space_guard(&lock);

	auto mapping = _findMapping(address);
	assert(mapping);

	// TODO: Allow shrinking of the mapping.
	assert(mapping->address() == address);
	assert(mapping->length() == length);
	mapping->uninstall();

	static constexpr auto deleteMapping = [] (AddressSpace *space, Mapping *mapping) {
		space->_mappings.remove(mapping);
		mapping->retire();
		mapping->selfPtr.ctr()->decrement();
	};

	static constexpr auto closeHole = [] (AddressSpace *space, VirtualAddr address, size_t length) {
		// Find the holes that preceede/succeede mapping.
		Hole *pre;
		Hole *succ;

		auto current = space->_holes.get_root();
		while(true) {
			assert(current);
			if(address < current->address()) {
				if(HoleTree::get_left(current)) {
					current = HoleTree::get_left(current);
				}else{
					pre = HoleTree::predecessor(current);
					succ = current;
					break;
				}
			}else{
				assert(address >= current->address() + current->length());
				if(HoleTree::get_right(current)) {
					current = HoleTree::get_right(current);
				}else{
					pre = current;
					succ = HoleTree::successor(current);
					break;
				}
			}
		}

		// Try to merge the new hole and the existing ones.
		if(pre && pre->address() + pre->length() == address
				&& succ && address + length == succ->address()) {
			auto hole = frigg::construct<Hole>(*kernelAlloc, pre->address(),
					pre->length() + length + succ->length());

			space->_holes.remove(pre);
			space->_holes.remove(succ);
			space->_holes.insert(hole);
			frigg::destruct(*kernelAlloc, pre);
			frigg::destruct(*kernelAlloc, succ);
		}else if(pre && pre->address() + pre->length() == address) {
			auto hole = frigg::construct<Hole>(*kernelAlloc,
					pre->address(), pre->length() + length);

			space->_holes.remove(pre);
			space->_holes.insert(hole);
			frigg::destruct(*kernelAlloc, pre);
		}else if(succ && address + length == succ->address()) {
			auto hole = frigg::construct<Hole>(*kernelAlloc,
					address, length + succ->length());

			space->_holes.remove(succ);
			space->_holes.insert(hole);
			frigg::destruct(*kernelAlloc, succ);
		}else{
			auto hole = frigg::construct<Hole>(*kernelAlloc,
					address, length);

			space->_holes.insert(hole);
		}
	};

	node->_worklet.setup([] (Worklet *base) {
		auto node = frg::container_of(base, &AddressUnmapNode::_worklet);

		auto irq_lock = frigg::guard(&irqMutex());
		AddressSpace::Guard space_guard(&node->_space->lock);

		deleteMapping(node->_space, node->_mapping.get());
		closeHole(node->_space, node->_shootNode.address, node->_shootNode.size);
		node->complete();
	});

	node->_space = this;
	node->_mapping = mapping;
	node->_shootNode.address = address;
	node->_shootNode.size = length;
	node->_shootNode.setup(&node->_worklet);
	if(!_pageSpace.submitShootdown(&node->_shootNode))
		return false;

	deleteMapping(this, mapping.get());
	closeHole(this, address, length);
	return true;
}

bool AddressSpace::handleFault(VirtualAddr address, uint32_t fault_flags, FaultNode *node) {
	node->_address = address;
	node->_flags = fault_flags;

	smarter::shared_ptr<Mapping> mapping;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		AddressSpace::Guard space_guard(&lock);

		mapping = _findMapping(address);
		if(!mapping) {
			node->_resolved = false;
			return true;
		}
	}

	node->_mapping = mapping;

	// Here we do the mapping-based fault handling.
	if(node->_flags & AddressSpace::kFaultWrite)
		if(!((mapping->flags() & MappingFlags::permissionMask) & MappingFlags::protWrite)) {
			node->_resolved = false;
			return true;
		}
	if(node->_flags & AddressSpace::kFaultExecute)
		if(!((mapping->flags() & MappingFlags::permissionMask) & MappingFlags::protExecute)) {
			node->_resolved = false;
			return true;
		}

	auto fault_page = (node->_address - mapping->address()) & ~(kPageSize - 1);
	node->_touchVirtual.setup(fault_page, &node->_worklet);
	node->_worklet.setup([] (Worklet *base) {
		auto node = frg::container_of(base, &FaultNode::_worklet);
		assert(!node->_touchVirtual.error());
		node->_resolved = true;
		WorkQueue::post(node->_handled);
	});
	if(mapping->touchVirtualPage(&node->_touchVirtual)) {
		if(node->_touchVirtual.error()) {
			node->_resolved = false;
			return true;
		}else{
			// Spurious page faults are the result of race conditions.
			// They should be rare. If they happen too often, something is probably wrong!
			if(node->_touchVirtual.spurious())
				frigg::infoLogger() << "\e[33m" "thor: Spurious page fault"
						<< "\e[39m" << frigg::endLog;
			node->_resolved = true;
			return true;
		}
	}else{
		return false;
	}
}

bool AddressSpace::fork(ForkNode *node) {
	node->_fork = AddressSpace::create();
	node->_original = this;

	// Lock the space and iterate over all holes and mappings.
	{
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&this->lock);

		// Copy holes to the child space.
		auto os_hole = _holes.first();
		while(os_hole) {
			auto fs_hole = frigg::construct<Hole>(*kernelAlloc,
					os_hole->address(), os_hole->length());
			node->_fork->_holes.insert(fs_hole);

			os_hole = HoleTree::successor(os_hole);
		}

		// Modify memory mapping of both spaces.
		auto os_mapping = _mappings.first();
		while(os_mapping) {
			if(os_mapping->flags() & MappingFlags::dropAtFork) {
				// TODO: Merge this hole into adjacent holes.
				auto fs_hole = frigg::construct<Hole>(*kernelAlloc,
						os_mapping->address(), os_mapping->length());
				node->_fork->_holes.insert(fs_hole);
			}else{
				auto fs_mapping = os_mapping->forkMapping();
				fs_mapping->tie(node->_fork->selfPtr.lock(), os_mapping->address());
				node->_fork->_mappings.insert(fs_mapping.get());
				fs_mapping->install();
				fs_mapping.release(); // AddressSpace owns one reference.

				// In the case of CoW, we need to perform shootdown.
				// TODO: Add not shoot down all mappings.
				node->_items.addBack(ForkItem{os_mapping});
			}

			os_mapping = MappingTree::successor(os_mapping);
		}
	}

	struct Ops {
		static bool process(ForkNode *node) {
			while(!node->_items.empty()) {
				auto item = &node->_items.front();

				node->_shootNode.address = item->mapping->address();
				node->_shootNode.size = item->mapping->length();
				node->_shootNode.setup(&node->_worklet);
				node->_worklet.setup([] (Worklet *base) {
					auto node = frg::container_of(base, &ForkNode::_worklet);
					node->_items.removeFront();

					// Tail of asynchronous path.
					if(!process(node))
						return;
					WorkQueue::post(node->_forked);
				});
				if(!node->_original->_pageSpace.submitShootdown(&node->_shootNode))
					return false;
				node->_items.removeFront();
			}

			return true;
		}
	};

	return Ops::process(node);
}

smarter::shared_ptr<Mapping> AddressSpace::_findMapping(VirtualAddr address) {
	auto current = _mappings.get_root();
	while(current) {
		if(address < current->address()) {
			current = MappingTree::get_left(current);
		}else if(address >= current->address() + current->length()) {
			current = MappingTree::get_right(current);
		}else{
			assert(address >= current->address()
					&& address < current->address() + current->length());
			return current->selfPtr.lock();
		}
	}

	return nullptr;
}

VirtualAddr AddressSpace::_allocate(size_t length, MapFlags flags) {
	assert(length > 0);
	assert((length % kPageSize) == 0);
//	frigg::infoLogger() << "Allocate virtual memory area"
//			<< ", size: 0x" << frigg::logHex(length) << frigg::endLog;

	if(_holes.get_root()->largestHole < length)
		return 0; // TODO: Return something else here?

	auto current = _holes.get_root();
	while(true) {
		if(flags & kMapPreferBottom) {
			// Try to allocate memory at the bottom of the range.
			if(HoleTree::get_left(current)
					&& HoleTree::get_left(current)->largestHole >= length) {
				current = HoleTree::get_left(current);
				continue;
			}

			if(current->length() >= length) {
				_splitHole(current, 0, length);
				return current->address();
			}

			assert(HoleTree::get_right(current));
			assert(HoleTree::get_right(current)->largestHole >= length);
			current = HoleTree::get_right(current);
		}else{
			// Try to allocate memory at the top of the range.
			assert(flags & kMapPreferTop);

			if(HoleTree::get_right(current)
					&& HoleTree::get_right(current)->largestHole >= length) {
				current = HoleTree::get_right(current);
				continue;
			}

			if(current->length() >= length) {
				size_t offset = current->length() - length;
				_splitHole(current, offset, length);
				return current->address() + offset;
			}

			assert(HoleTree::get_left(current));
			assert(HoleTree::get_left(current)->largestHole >= length);
			current = HoleTree::get_left(current);
		}
	}
}

VirtualAddr AddressSpace::_allocateAt(VirtualAddr address, size_t length) {
	assert(!(address % kPageSize));
	assert(!(length % kPageSize));

	auto current = _holes.get_root();
	while(true) {
		// TODO: Otherwise, this method fails.
		assert(current);

		if(address < current->address()) {
			current = HoleTree::get_left(current);
		}else if(address >= current->address() + current->length()) {
			current = HoleTree::get_right(current);
		}else{
			assert(address >= current->address()
					&& address < current->address() + current->length());
			break;
		}
	}

	_splitHole(current, address - current->address(), length);
	return address;
}

void AddressSpace::_splitHole(Hole *hole, VirtualAddr offset, size_t length) {
	assert(length);
	assert(offset + length <= hole->length());

	_holes.remove(hole);

	if(offset) {
		auto predecessor = frigg::construct<Hole>(*kernelAlloc, hole->address(), offset);
		_holes.insert(predecessor);
	}

	if(offset + length < hole->length()) {
		auto successor = frigg::construct<Hole>(*kernelAlloc,
				hole->address() + offset + length, hole->length() - (offset + length));
		_holes.insert(successor);
	}

	frigg::destruct(*kernelAlloc, hole);
}

// --------------------------------------------------------
// MemoryViewLockHandle.
// --------------------------------------------------------

MemoryViewLockHandle::MemoryViewLockHandle(frigg::SharedPtr<MemoryView> view,
		uintptr_t offset, size_t size)
: _view{std::move(view)}, _offset{offset}, _size{size} {
	if(auto e = _view->lockRange(_offset, _size); e)
		return;
	_active = true;
}

MemoryViewLockHandle::~MemoryViewLockHandle() {
	if(_active)
		_view->unlockRange(_offset, _size);
}

// --------------------------------------------------------
// AddressSpaceLockHandle
// --------------------------------------------------------

AddressSpaceLockHandle::AddressSpaceLockHandle(smarter::shared_ptr<AddressSpace, BindableHandle> space,
		void *pointer, size_t length)
: _space{std::move(space)}, _address{reinterpret_cast<uintptr_t>(pointer)}, _length{length} {
	if(!_length)
		return;
	assert(_address);

	// TODO: Verify the mapping's size.
	_mapping = _space->getMapping(_address);
	assert(_mapping);
}

AddressSpaceLockHandle::~AddressSpaceLockHandle() {
	if(!_length)
		return;

	if(_active)
		_mapping->unlockVirtualRange(_address - _mapping->address(), _length);
}

bool AddressSpaceLockHandle::acquire(AcquireNode *node) {
	if(!_length) {
		_active = true;
		return true;
	}

	node->_accessor = this;

	struct Ops {
		static bool doLock(AcquireNode *node) {
			auto self = node->_accessor;
			node->_lockNode.setup(self->_address - self->_mapping->address(), self->_length,
					&node->_worklet);
			node->_worklet.setup([] (Worklet *base) {
				auto node = frg::container_of(base, &AcquireNode::_worklet);
				auto self = node->_accessor;
				if(doPopulate(node)) {
					self->_active = true;
					WorkQueue::post(node->_acquired);
				}
			});
			if(!self->_mapping->lockVirtualRange(&node->_lockNode))
				return false;
			return doPopulate(node);
		}

		static bool doPopulate(AcquireNode *node) {
			auto self = node->_accessor;
			node->_populateNode.setup(self->_address - self->_mapping->address(), self->_length,
					&node->_worklet);
			node->_worklet.setup([] (Worklet *base) {
				auto node = frg::container_of(base, &AcquireNode::_worklet);
				auto self = node->_accessor;
				self->_active = true;
				WorkQueue::post(node->_acquired);
			});
			if(!self->_mapping->populateVirtualRange(&node->_populateNode))
				return false;
			return true;
		}
	};

	if(!Ops::doLock(node))
		return false;

	_active = true;
	return true;
}

PhysicalAddr AddressSpaceLockHandle::getPhysical(size_t offset) {
	return _resolvePhysical(_address + offset);
}

void AddressSpaceLockHandle::load(size_t offset, void *pointer, size_t size) {
	assert(_active);
	assert(offset + size <= _length);

	size_t progress = 0;
	while(progress < size) {
		VirtualAddr write = (VirtualAddr)_address + offset + progress;
		size_t misalign = (VirtualAddr)write % kPageSize;
		size_t chunk = frigg::min(kPageSize - misalign, size - progress);

		PhysicalAddr page = _resolvePhysical(write - misalign);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{page};
		memcpy((char *)pointer + progress, (char *)accessor.get() + misalign, chunk);
		progress += chunk;
	}
}

Error AddressSpaceLockHandle::write(size_t offset, const void *pointer, size_t size) {
	assert(_active);
	assert(offset + size <= _length);

	size_t progress = 0;
	while(progress < size) {
		VirtualAddr write = (VirtualAddr)_address + offset + progress;
		size_t misalign = (VirtualAddr)write % kPageSize;
		size_t chunk = frigg::min(kPageSize - misalign, size - progress);

		PhysicalAddr page = _resolvePhysical(write - misalign);
		assert(page != PhysicalAddr(-1));

		PageAccessor accessor{page};
		memcpy((char *)accessor.get() + misalign, (char *)pointer + progress, chunk);
		progress += chunk;
	}

	return kErrSuccess;
}

PhysicalAddr AddressSpaceLockHandle::_resolvePhysical(VirtualAddr vaddr) {
	auto range = _mapping->resolveRange(vaddr - _mapping->address());
	return range.get<0>();
}

// --------------------------------------------------------
// NamedMemoryViewLock.
// --------------------------------------------------------

NamedMemoryViewLock::~NamedMemoryViewLock() { }

} // namespace thor

