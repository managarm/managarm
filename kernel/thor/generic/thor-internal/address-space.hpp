#pragma once

#include <async/basic.hpp>
#include <async/mutex.hpp>
#include <async/oneshot-event.hpp>
#include <frg/container_of.hpp>
#include <frg/expected.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/memory-view.hpp>
#include <thor-internal/mm-rc.hpp>

namespace thor {

struct GlobalFutex {
	GlobalFutex(FutexIdentity id, PhysicalAddr physical)
	: id_{id}, physical_{physical} {
		assert(!(physical & (sizeof(int) - 1)));
	}

	FutexIdentity getIdentity() {
		return id_;
	}

	unsigned int read() {
		PageAccessor accessor{physical_ & ~(kPageSize - 1)};
		auto offset = physical_ & (kPageSize - 1);
		auto accessPtr = reinterpret_cast<unsigned int *>(
				reinterpret_cast<std::byte *>(accessor.get()) + offset);
		return __atomic_load_n(accessPtr, __ATOMIC_RELAXED);
	}

private:
	FutexIdentity id_;
	PhysicalAddr physical_;
};

struct VirtualSpace;

inline CachingMode determineCachingMode(CachingMode physicalRangeCaching,
		CachingMode requested) {
	// check if an override caching mode was requested
	if(requested != CachingMode::null) {
		// allow overriding UC with WC
		if(requested == CachingMode::writeCombine && physicalRangeCaching == CachingMode::uncached) {
			return requested;
		}
	}

	// otherwise, return the caching mode from the physical range
	return physicalRangeCaching;
}

template<typename Cursor, typename PageSpace>
frg::expected<Error> mapPresentPagesByCursor(PageSpace *ps, VirtualAddr va,
		MemoryView *view, uintptr_t offset, size_t size, PageFlags flags, CachingMode mode) {
	assert(!(va & (kPageSize - 1)));
	assert(!(offset & (kPageSize - 1)));
	assert(!(size & (kPageSize - 1)));

	Cursor c{ps, va};
	while(c.virtualAddress() < va + size) {
		auto progress = c.virtualAddress() - va;
		auto physicalRange = view->peekRange(offset + progress);
		if(physicalRange.template get<0>() == PhysicalAddr(-1)) {
			c.advance4k();
			continue;
		}
		assert(!(physicalRange.template get<0>() & (kPageSize - 1)));

		c.map4k(physicalRange.template get<0>(), flags,
			determineCachingMode(physicalRange.template get<1>(), mode));
		c.advance4k();
	}
	return {};
}

template<typename Cursor, typename PageSpace>
frg::expected<Error> remapPresentPagesByCursor(PageSpace *ps, VirtualAddr va,
		MemoryView *view, uintptr_t offset, size_t size, PageFlags flags, CachingMode mode) {
	assert(!(va & (kPageSize - 1)));
	assert(!(offset & (kPageSize - 1)));
	assert(!(size & (kPageSize - 1)));

	Cursor c{ps, va};
	while(c.virtualAddress() < va + size) {
		auto progress = c.virtualAddress() - va;

		auto physicalRange = view->peekRange(offset + progress);
		if(physicalRange.template get<0>() == PhysicalAddr(-1)) {
			auto [status, _] = c.unmap4k();
			if((status & page_status::present) && (status & page_status::dirty)) {
				view->markDirty(offset + progress, kPageSize);
			}

			c.advance4k();
			continue;
		}
		assert(!(physicalRange.template get<0>() & (kPageSize - 1)));

		auto status = c.remap4k(physicalRange.template get<0>(), flags,
			determineCachingMode(physicalRange.template get<1>(), mode));
		c.advance4k();

		if((status & page_status::present) && (status & page_status::dirty)) {
			view->markDirty(offset + progress, kPageSize);
		}
	}
	return {};
}

template<typename Cursor, typename PageSpace>
frg::expected<Error> faultPageByCursor(PageSpace *ps, VirtualAddr va,
		MemoryView *view, uintptr_t offset, PageFlags flags, CachingMode mode) {
	assert(!(va & (kPageSize - 1)));
	assert(!(offset & (kPageSize - 1)));

	Cursor c{ps, va};

	auto physicalRange = view->peekRange(offset);
	if(physicalRange.get<0>() == PhysicalAddr(-1))
		return Error::fault;

	auto status = c.remap4k(physicalRange.template get<0>(), flags,
		determineCachingMode(physicalRange.template get<1>(), mode));
	if(status & page_status::present) {
		if(status & page_status::dirty)
			view->markDirty(offset, kPageSize);
	}

	return {};
}

template<typename Cursor, typename PageSpace>
frg::expected<Error> cleanPagesByCursor(PageSpace *ps, VirtualAddr va,
		MemoryView *view, uintptr_t offset, size_t size) {
	assert(!(va & (kPageSize - 1)));
	assert(!(offset & (kPageSize - 1)));
	assert(!(size & (kPageSize - 1)));

	Cursor c{ps, va};
	while(c.findDirty(va + size)) {
		auto progress = c.virtualAddress() - va;

		auto status = c.clean4k();
		assert(status & page_status::present);
		assert(status & page_status::dirty);
		view->markDirty(offset + progress, kPageSize);

		c.advance4k();
	}
	return {};
}

template<typename Cursor, typename PageSpace>
frg::expected<Error> unmapPagesByCursor(PageSpace *ps, VirtualAddr va,
		MemoryView *view, uintptr_t offset, size_t size) {
	assert(!(va & (kPageSize - 1)));
	assert(!(offset & (kPageSize - 1)));
	assert(!(size & (kPageSize - 1)));

	Cursor c{ps, va};
	while(c.findPresent(va + size)) {
		auto progress = c.virtualAddress() - va;

		auto [status, _] = c.unmap4k();
		assert(status & page_status::present);
		if(status & page_status::dirty)
			view->markDirty(offset + progress, kPageSize);

		c.advance4k();
	}
	return {};
}

struct VirtualOperations {
	virtual void retire(RetireNode *node) = 0;

	virtual bool submitShootdown(ShootNode *node) = 0;

	virtual void mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
			uint32_t flags, CachingMode cachingMode) {
		(void)pointer;
		(void)physical;
		(void)flags;
		(void)cachingMode;
		panicLogger() << "thor: Default VirtualOperations::mapSingle4k called!" << frg::endlog;
		__builtin_unreachable();
	}
	virtual PageStatus unmapSingle4k(VirtualAddr pointer) {
		(void)pointer;
		panicLogger() << "thor: Default VirtualOperations::unmapSingle4k called!" << frg::endlog;
		__builtin_unreachable();
	}
	virtual PageStatus cleanSingle4k(VirtualAddr pointer) {
		(void)pointer;
		panicLogger() << "thor: Default VirtualOperations::cleanSingle4k called!" << frg::endlog;
		__builtin_unreachable();
	}
	virtual bool isMapped(VirtualAddr pointer) {
		(void)pointer;
		panicLogger() << "thor: Default VirtualOperations::isMapped called!" << frg::endlog;
		__builtin_unreachable();
	}

	// ----------------------------------------------------------------------------------

	// The following API is based on MemoryView and will replace the legacy API above.
	// The advantage of this approach is that we do not need on virtual call per page anymore.

	virtual frg::expected<Error> mapPresentPages(VirtualAddr va, MemoryView *view,
			uintptr_t offset, size_t size, PageFlags flags, CachingMode mode);

	virtual frg::expected<Error> remapPresentPages(VirtualAddr va, MemoryView *view,
			uintptr_t offset, size_t size, PageFlags flags, CachingMode mode);

	virtual frg::expected<Error> faultPage(VirtualAddr va, MemoryView *view,
			uintptr_t offset, PageFlags flags, CachingMode mode);

	virtual frg::expected<Error> cleanPages(VirtualAddr va, MemoryView *view,
			uintptr_t offset, size_t size);

	virtual frg::expected<Error> unmapPages(VirtualAddr va, MemoryView *view,
			uintptr_t offset, size_t size);

	virtual size_t getRss();

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for retire()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct RetireOperation final : private RetireNode {
		RetireOperation(VirtualOperations *self, WorkQueue *wq, R receiver)
		: self_{self}, wq_{wq}, receiver_{std::move(receiver)} { }

		void start() {
			RetireNode::wq_ = wq_;
			Worklet::setup([] (Worklet *base) {
				auto op = static_cast<RetireOperation *>(base);
				async::execution::set_value(op->receiver_);
			});
			self_->retire(this);
		}

	private:
		VirtualOperations *self_;
		WorkQueue *wq_;
		R receiver_;
	};

	struct RetireSender {
		using value_type = void;

		template<typename R>
		RetireOperation<R> connect(R receiver) {
			return {self, wq, std::move(receiver)};
		}

		async::sender_awaiter<RetireSender> operator co_await() {
			return {*this};
		}

		VirtualOperations *self;
		WorkQueue *wq;
	};

	RetireSender retire(WorkQueue *wq) {
		return {this, wq};
	}

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for shootdown()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct ShootdownOperation;

	struct [[nodiscard]] ShootdownSender {
		using value_type = void;

		template<typename R>
		friend ShootdownOperation<R>
		connect(ShootdownSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		VirtualOperations *self;
		VirtualAddr address;
		size_t size;
		WorkQueue *wq;
	};

	ShootdownSender shootdown(VirtualAddr address, size_t size, WorkQueue *wq) {
		return {this, address, size, wq};
	}

	template<typename R>
	struct ShootdownOperation final : private ShootNode {
		ShootdownOperation(ShootdownSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		ShootdownOperation(const ShootdownOperation &) = delete;

		ShootdownOperation &operator= (const ShootdownOperation &) = delete;

		void start() {
			ShootNode::address = s_.address;
			ShootNode::size = s_.size;
			ShootNode::wq_ = s_.wq;
			Worklet::setup([] (Worklet *base) {
				auto op = static_cast<ShootdownOperation *>(base);
				async::execution::set_value(op->receiver_);
			});
			if(s_.self->submitShootdown(this))
				return async::execution::set_value(receiver_);
		}

	private:
		ShootdownSender s_;
		R receiver_;
	};

	friend async::sender_awaiter<ShootdownSender>
	operator co_await(ShootdownSender sender) {
		return {sender};
	}

protected:
	~VirtualOperations() = default;

	// ----------------------------------------------------------------------------------
};

struct Hole {
	Hole(VirtualAddr address, size_t length)
	: _address{address}, _length{length}, largestHole{0} { }

	VirtualAddr address() const {
		return _address;
	}

	size_t length() const {
		return _length;
	}

	frg::rbtree_hook treeNode;

private:
	VirtualAddr _address;
	size_t _length;

public:
	// Largest hole in the subtree of this node.
	size_t largestHole;
};

enum MappingFlags : uint32_t {
	null = 0,

	permissionMask = 0x70,
	protRead = 0x10,
	protWrite = 0x20,
	protExecute = 0x40,

	dontRequireBacking = 0x100
};

struct TouchVirtualResult {
	PhysicalRange range;
	bool spurious;
};

enum class MappingState {
	null,
	active,
	zombie,
	retired
};

struct Mapping {
	Mapping(size_t length, MappingFlags flags,
			smarter::shared_ptr<MemorySlice> view, uintptr_t offset);

	Mapping(const Mapping &) = delete;

	~Mapping();

	Mapping &operator= (const Mapping &) = delete;

	void tie(smarter::shared_ptr<VirtualSpace> owner, VirtualAddr address);

	void protect(MappingFlags flags);

	frg::tuple<PhysicalAddr, CachingMode>
	resolveRange(ptrdiff_t offset);

	smarter::borrowed_ptr<Mapping> selfPtr;

	frg::rbtree_hook treeNode;

	uint32_t compilePageFlags();

	coroutine<void> runEvictionLoop();

	smarter::shared_ptr<VirtualSpace> owner;
	VirtualAddr address;
	size_t length;
	MappingFlags flags;

	MappingState state = MappingState::null;
	MemoryObserver observer;

	// This (asynchronous) mutex can be used to temporarily disable eviction.
	// By disabling eviction, we can safely map pages returned from peekRange()
	// before they can be evicted.
	async::mutex evictionMutex;

	async::cancellation_event cancelEviction;
	async::oneshot_event evictionDoneEvent;
	smarter::shared_ptr<MemorySlice> slice;
	smarter::shared_ptr<MemoryView> view;
	size_t viewOffset;

	// This mutex is held whenever we modify parts of the page space that belong
	// to this mapping (using VirtualOperation::mapSingle4k and similar). This is
	// necessary since we sometimes need to read pages before writing them.
	frg::ticket_spinlock pagingMutex;
};

struct HoleLess {
	bool operator() (const Hole &a, const Hole &b) {
		return a.address() < b.address();
	}
};

struct HoleAggregator;

using HoleTree = frg::rbtree<
	Hole,
	&Hole::treeNode,
	HoleLess,
	HoleAggregator
>;

struct HoleAggregator {
	static bool aggregate(Hole *node);
	static bool check_invariant(HoleTree &tree, Hole *node);
};

struct MappingLess {
	bool operator() (const Mapping &a, const Mapping &b) {
		return a.address < b.address;
	}
};

using MappingTree = frg::rbtree<
	Mapping,
	&Mapping::treeNode,
	MappingLess
>;

struct VirtualSpace {
	friend struct Mapping;

public:
	typedef uint32_t MapFlags;
	enum : MapFlags {
		kMapFixed = 0x01,
		kMapPreferBottom = 0x02,
		kMapPreferTop = 0x04,
		kMapProtRead = 0x08,
		kMapProtWrite = 0x10,
		kMapProtExecute = 0x20,
		kMapPopulate = 0x200,
		kMapDontRequireBacking = 0x400,
		kMapFixedNoReplace = 0x800
	};

	enum FaultFlags : uint32_t {
		kFaultWrite = (1 << 1),
		kFaultExecute = (1 << 2)
	};

	VirtualSpace(VirtualOperations *ops);

	~VirtualSpace();

	void retire();

	void setupInitialHole(VirtualAddr address, size_t size);

	coroutine<frg::expected<Error, VirtualAddr>>
	map(smarter::borrowed_ptr<MemorySlice> view,
			VirtualAddr address, size_t offset, size_t length, uint32_t flags,
			WorkQueue *wq);

	coroutine<frg::expected<Error>>
	protect(VirtualAddr address, size_t length, uint32_t flags, WorkQueue *wq);

	coroutine<frg::expected<Error>>
	synchronize(VirtualAddr address, size_t length, WorkQueue *wq);

	coroutine<frg::expected<Error>>
	unmap(VirtualAddr address, size_t length, WorkQueue *wq);

	coroutine<frg::expected<Error>>
	handleFault(VirtualAddr address, uint32_t flags, WorkQueue *wq);

	coroutine<frg::expected<Error, PhysicalAddr>>
	retrievePhysical(VirtualAddr address, WorkQueue *wq);

	size_t rss() {
		return _ops->getRss();
	}

	// ----------------------------------------------------------------------------------
	// Read/write support.
	// ----------------------------------------------------------------------------------

	// These functions read as much data as possible;
	// on error, they read/write a partially filled buffer.
	coroutine<size_t> readPartialSpace(uintptr_t address, void *buffer, size_t size,
			WorkQueue *wq);
	coroutine<size_t> writePartialSpace(uintptr_t address, const void *buffer, size_t size,
			WorkQueue *wq);

	auto readSpace(uintptr_t address, void *buffer, size_t size,
			WorkQueue *wq) {
		return async::transform(
			readPartialSpace(address, buffer, size, wq),
			[=] (size_t actualSize) -> frg::expected<Error> {
				if(actualSize != size)
					return Error::fault;
				return {};
			}
		);
	}

	auto writeSpace(uintptr_t address, const void *buffer, size_t size,
			WorkQueue *wq) {
		return async::transform(
			writePartialSpace(address, buffer, size, wq),
			[=] (size_t actualSize) -> frg::expected<Error> {
				if(actualSize != size)
					return Error::fault;
				return {};
			}
		);
	}

	// ----------------------------------------------------------------------------------
	// GlobalFutex support.
	// ----------------------------------------------------------------------------------

	struct GlobalFutexSpace {
		template<typename F>
		coroutine<frg::expected<Error>> withFutex(uintptr_t address, WorkQueue *, F &&f) {
			assert(currentIpl() == ipl::exceptional);

			if (address & (sizeof(int) - 1))
				co_return Error::illegalArgs;

			while (true) {
				smarter::shared_ptr<Mapping> mapping;
				{
					auto irqLock = frg::guard(&irqMutex());
					auto spaceGuard = frg::guard(&self->_snapshotMutex);
					mapping = self->_findMapping(address);
				}
				if(!mapping)
					co_return Error::fault;

				auto offset = address - mapping->address;
				auto alignedOffset = offset & ~(kPageSize - 1);
				auto offsetMisalign = offset & (kPageSize - 1);

				// TODO: We may want to resolve the page to a (owner MemoryView, offset) pair
				//       to handle futexes behind IndirectMemory.
				//       However, we do not have any futexes on IndirectMemory right now.
				FutexIdentity id{
					.spaceQualifier = reinterpret_cast<uintptr_t>(mapping->view.get()),
					.localAddress = mapping->viewOffset + offset,
				};

				// Lock evictionMutex to prevent page eviction.
				{
					co_await mapping->evictionMutex.async_lock();
					frg::unique_lock evictionLock{frg::adopt_lock, mapping->evictionMutex};

					// Complete the operation if the memory page is available.
					auto [physical, caching] = mapping->view->peekRange(mapping->viewOffset + alignedOffset);
					if(physical != PhysicalAddr(-1)) {
						f(GlobalFutex{id, physical + offsetMisalign});
						co_return {};
					}
				}

				// Otherwise, try to make the page available.
				FRG_CO_TRY(co_await mapping->view->touchRange(
					alignedOffset, kPageSize
				));
			}
		}

		VirtualSpace *self;
	};
	static_assert(FutexSpace<GlobalFutexSpace>);

	GlobalFutexSpace globalFutexSpace() {
		return {this};
	}

	// ----------------------------------------------------------------------------------

	smarter::borrowed_ptr<VirtualSpace> selfPtr;

private:
	// Allocates a new mapping of the given length somewhere in the address space.
	frg::expected<Error, VirtualAddr> _allocate(size_t length, MapFlags flags);

	frg::expected<Error, VirtualAddr> _allocateAt(VirtualAddr address, size_t length);

	smarter::shared_ptr<Mapping> _findMapping(VirtualAddr address);

	bool _areMappingsInRange(VirtualAddr address, VirtualAddr length);

	// Splits some memory range from a hole mapping.
	void _splitHole(Hole *hole, VirtualAddr offset, VirtualAddr length);

	// Potentially splits mappings into two parts at (address) and (address + size).
	// Returns the start and end mappings that are within the specified range.
	coroutine<frg::tuple<Mapping *, Mapping *>> _splitMappings(uintptr_t address, size_t size);

	// Used in conjunction with _splitMappings.
	// Unmaps and removes all mappings between start and end that fall within the specified range.
	// Returns whether shootdown needs to be performed (any of the mappings got unmapped).
	coroutine<bool> _unmapMappings(VirtualAddr address, size_t length, Mapping *start, Mapping *end);

	VirtualOperations *_ops;

	// Since changing memory mappings requires TLB shootdown, most mapping-related operations
	// of VirtualSpace are async. Thus, we use an async mutex to serialize these operations.
	async::shared_mutex _consistencyMutex;

	// To avoid taking _consistencyMutex for operations that only need to look at the current
	// state of the VirtualSpace (and that can run concurrently with mapping-related that
	// perform TLB shootdown), we have another mutex that only protects _holes and _mappings.
	// We make sure that we "commit" changes to _holes and _mappings before changing page
	// tables and/or doing TLB shootdown.
	frg::ticket_spinlock _snapshotMutex;

	HoleTree _holes;
	MappingTree _mappings;
};

struct AddressSpace final : VirtualSpace, smarter::crtp_counter<AddressSpace, BindableHandle> {
	friend struct Mapping;

	// Silence Clang warning about hidden overloads.
	using smarter::crtp_counter<AddressSpace, BindableHandle>::dispose;

	struct Operations final : VirtualOperations {
		Operations(AddressSpace *space)
		: space_{space} { }

		void retire(RetireNode *node) override {
			return space_->pageSpace_.retire(node);
		}

		bool submitShootdown(ShootNode *node) override {
			return space_->pageSpace_.submitShootdown(node);
		}

		frg::expected<Error> mapPresentPages(VirtualAddr va, MemoryView *view,
				uintptr_t offset, size_t size, PageFlags flags, CachingMode mode) override {
			return mapPresentPagesByCursor<ClientPageSpace::Cursor>(&space_->pageSpace_,
					va, view, offset, size, flags, mode);
		}

		frg::expected<Error> remapPresentPages(VirtualAddr va, MemoryView *view,
				uintptr_t offset, size_t size, PageFlags flags, CachingMode mode) override {
			return remapPresentPagesByCursor<ClientPageSpace::Cursor>(&space_->pageSpace_,
					va, view, offset, size, flags, mode);
		}

		frg::expected<Error> faultPage(VirtualAddr va, MemoryView *view,
				uintptr_t offset, PageFlags flags, CachingMode mode) override {
			return faultPageByCursor<ClientPageSpace::Cursor>(&space_->pageSpace_,
					va, view, offset, flags, mode);
		}

		frg::expected<Error> cleanPages(VirtualAddr va, MemoryView *view,
				uintptr_t offset, size_t size) override {
			return cleanPagesByCursor<ClientPageSpace::Cursor>(&space_->pageSpace_,
					va, view, offset, size);
		}

		frg::expected<Error> unmapPages(VirtualAddr va, MemoryView *view,
				uintptr_t offset, size_t size) override {
			return unmapPagesByCursor<ClientPageSpace::Cursor>(&space_->pageSpace_,
					va, view, offset, size);
		}

	private:
		AddressSpace *space_;
	};

public:
	static smarter::shared_ptr<AddressSpace, BindableHandle>
	constructHandle(smarter::shared_ptr<AddressSpace> ptr) {
		auto space = ptr.get();
		space->setup(smarter::adopt_rc, ptr.ctr(), 1);
		ptr.release();
		return smarter::shared_ptr<AddressSpace, BindableHandle>{smarter::adopt_rc, space, space};
	}

	static smarter::shared_ptr<AddressSpace, BindableHandle> create() {
		auto ptr = smarter::allocate_shared<AddressSpace>(Allocator{});
		ptr->selfPtr = ptr;
		ptr->setupInitialHole(0x1000, (UINT64_C(1) << getLowerHalfBits()) - 0x1000);
		return constructHandle(std::move(ptr));
	}

	static void activate(smarter::shared_ptr<AddressSpace, BindableHandle> space);

	AddressSpace();

	~AddressSpace();

	void dispose(BindableHandle);

	FutexRealm localFutexRealm;

	bool updatePageAccess(VirtualAddr address, PageFlags flags) {
		return pageSpace_.updatePageAccess(address, flags);
	}

private:
	Operations ops_;
	ClientPageSpace pageSpace_;
};

struct MemoryViewLockHandle {
	friend void swap(MemoryViewLockHandle &a, MemoryViewLockHandle &b) {
		using std::swap;
		swap(a._view, b._view);
		swap(a._offset, b._offset);
		swap(a._size, b._size);
		swap(a._active, b._active);
	}

	MemoryViewLockHandle() = default;

	MemoryViewLockHandle(smarter::shared_ptr<MemoryView> view, uintptr_t offset, size_t size)
	: _view{view}, _offset{offset}, _size{size}, _active{false} { }

	MemoryViewLockHandle(const MemoryViewLockHandle &) = delete;

	MemoryViewLockHandle(MemoryViewLockHandle &&other)
	: MemoryViewLockHandle{} {
		swap(*this, other);
	}

	~MemoryViewLockHandle();

	MemoryViewLockHandle &operator= (MemoryViewLockHandle other) {
		swap(*this, other);
		return *this;
	}

	explicit operator bool () {
		return _active;
	}

	void acquire() {
		assert(!_active);

		auto err = _view->lockRange(_offset, _size);
		if(err != Error::success)
			return;
		_active = true;
	}

private:
	smarter::shared_ptr<MemoryView> _view = nullptr;
	uintptr_t _offset = 0;
	size_t _size = 0;
	bool _active = false;
};

struct NamedMemoryViewLock {
	NamedMemoryViewLock(MemoryViewLockHandle handle)
	: _handle{std::move(handle)} { }

	NamedMemoryViewLock(const NamedMemoryViewLock &) = delete;

	~NamedMemoryViewLock();

	NamedMemoryViewLock &operator= (const NamedMemoryViewLock &) = delete;

private:
	MemoryViewLockHandle _handle;
};

void initializeReclaim();

} // namespace thor
