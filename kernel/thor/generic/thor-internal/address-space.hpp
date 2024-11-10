#pragma once

#include <async/basic.hpp>
#include <async/mutex.hpp>
#include <async/oneshot-event.hpp>
#include <frg/container_of.hpp>
#include <frg/expected.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/memory-view.hpp>

namespace thor {

struct VirtualSpace;

template<typename Cursor, typename PageSpace>
frg::expected<Error> mapPresentPagesByCursor(PageSpace *ps, VirtualAddr va,
		MemoryView *view, uintptr_t offset, size_t size, PageFlags flags) {
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

		c.map4k(physicalRange.template get<0>(), flags, physicalRange.template get<1>());
		c.advance4k();
	}
	return {};
}

template<typename Cursor, typename PageSpace>
frg::expected<Error> remapPresentPagesByCursor(PageSpace *ps, VirtualAddr va,
		MemoryView *view, uintptr_t offset, size_t size, PageFlags flags) {
	assert(!(va & (kPageSize - 1)));
	assert(!(offset & (kPageSize - 1)));
	assert(!(size & (kPageSize - 1)));

	Cursor c{ps, va};
	while(c.virtualAddress() < va + size) {
		auto progress = c.virtualAddress() - va;

		auto status = c.unmap4k();
		if((status & page_status::present) && (status & page_status::dirty)) {
			view->markDirty(offset + progress, kPageSize);
		}

		auto physicalRange = view->peekRange(offset + progress);
		if(physicalRange.template get<0>() == PhysicalAddr(-1)) {
			c.advance4k();
			continue;
		}
		assert(!(physicalRange.template get<0>() & (kPageSize - 1)));

		c.map4k(physicalRange.template get<0>(), flags, physicalRange.template get<1>());
		c.advance4k();
	}
	return {};
}

template<typename Cursor, typename PageSpace>
frg::expected<Error> faultPageByCursor(PageSpace *ps, VirtualAddr va,
		MemoryView *view, uintptr_t offset, PageFlags flags) {
	assert(!(va & (kPageSize - 1)));
	assert(!(offset & (kPageSize - 1)));

	Cursor c{ps, va};

	auto physicalRange = view->peekRange(offset);
	if(physicalRange.get<0>() == PhysicalAddr(-1))
		return Error::fault;

	auto status = c.remap4k(physicalRange.template get<0>(), flags, physicalRange.template get<1>());
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

		auto status = c.unmap4k();
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
			uintptr_t offset, size_t size, PageFlags flags);

	virtual frg::expected<Error> remapPresentPages(VirtualAddr va, MemoryView *view,
			uintptr_t offset, size_t size, PageFlags flags);

	virtual frg::expected<Error> faultPage(VirtualAddr va, MemoryView *view,
			uintptr_t offset, PageFlags flags);

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
		RetireOperation(VirtualOperations *self, R receiver)
		: self_{self}, receiver_{std::move(receiver)} { }

		void start() {
			self_->retire(this);
		}

	private:
		void complete() override {
			async::execution::set_value(receiver_);
		}

		VirtualOperations *self_;
		R receiver_;
	};

	struct RetireSender {
		template<typename R>
		RetireOperation<R> connect(R receiver) {
			return {self, std::move(receiver)};
		}

		async::sender_awaiter<RetireSender> operator co_await() {
			return {*this};
		}

		VirtualOperations *self;
	};

	RetireSender retire() {
		return {this};
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
	};

	ShootdownSender shootdown(VirtualAddr address, size_t size) {
		return {this, address, size};
	}

	template<typename R>
	struct ShootdownOperation final : private ShootNode {
		ShootdownOperation(ShootdownSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		ShootdownOperation(const ShootdownOperation &) = delete;

		ShootdownOperation &operator= (const ShootdownOperation &) = delete;

		bool start_inline() {
			ShootNode::address = s_.address;
			ShootNode::size = s_.size;
			if(s_.self->submitShootdown(this)) {
				async::execution::set_value_inline(receiver_);
				return true;
			}
			return false;
		}

	private:
		void complete() override {
			async::execution::set_value_noinline(receiver_);
		}

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

	void unlockVirtualRange(uintptr_t offset, size_t length);

	frg::tuple<PhysicalAddr, CachingMode>
	resolveRange(ptrdiff_t offset);

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for lockVirtualRange()
	// ----------------------------------------------------------------------------------
private:
	struct LockVirtualRangeNode {
		virtual void resume() = 0;

		frg::expected<Error> result;

	protected:
		~LockVirtualRangeNode() = default;
	};

	// Makes sure that pages are not evicted from virtual memory.
	void lockVirtualRange(uintptr_t offset, size_t length,
			smarter::shared_ptr<WorkQueue> wq, LockVirtualRangeNode *node);

public:
	template<typename R>
	struct LockVirtualRangeOperation;

	struct [[nodiscard]] LockVirtualRangeSender {
		template<typename R>
		friend LockVirtualRangeOperation<R>
		connect(LockVirtualRangeSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		Mapping *self;
		uintptr_t offset;
		size_t size;
		smarter::shared_ptr<WorkQueue> wq;
	};

	LockVirtualRangeSender lockVirtualRange(uintptr_t offset, size_t size,
			smarter::shared_ptr<WorkQueue> wq) {
		return {this, offset, size, std::move(wq)};
	}

	template<typename R>
	struct LockVirtualRangeOperation final : private LockVirtualRangeNode {
		LockVirtualRangeOperation(LockVirtualRangeSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		LockVirtualRangeOperation(const LockVirtualRangeOperation &) = delete;

		LockVirtualRangeOperation &operator= (const LockVirtualRangeOperation &) = delete;

		void start() {
			// XXX: work around Clang bug that runs s_.wq dtor after the call.
			auto wq = s_.wq;
			s_.self->lockVirtualRange(s_.offset, s_.size, std::move(wq), this);
		}

	private:
		void resume() override {
			async::execution::set_value(receiver_, result);
		}

		LockVirtualRangeSender s_;
		R receiver_;
	};

	friend async::sender_awaiter<LockVirtualRangeSender, frg::expected<Error>>
	operator co_await(LockVirtualRangeSender sender) {
		return {sender};
	}

	// ----------------------------------------------------------------------------------

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
			VirtualAddr address, size_t offset, size_t length, uint32_t flags);

	coroutine<frg::expected<Error>>
	protect(VirtualAddr address, size_t length, uint32_t flags);

	coroutine<frg::expected<Error>>
	synchronize(VirtualAddr address, size_t length);

	coroutine<frg::expected<Error>>
	unmap(VirtualAddr address, size_t length);

	coroutine<frg::expected<Error>>
	handleFault(VirtualAddr address, uint32_t flags, smarter::shared_ptr<WorkQueue> wq);

	coroutine<frg::expected<Error, PhysicalAddr>>
	retrievePhysical(VirtualAddr address, smarter::shared_ptr<WorkQueue> wq);

	size_t rss() {
		return _ops->getRss();
	}

	// ----------------------------------------------------------------------------------
	// Read/write support.
	// ----------------------------------------------------------------------------------

	// These functions read as much data as possible;
	// on error, they read/write a partially filled buffer.
	coroutine<size_t> readPartialSpace(uintptr_t address, void *buffer, size_t size,
			smarter::shared_ptr<WorkQueue> wq);
	coroutine<size_t> writePartialSpace(uintptr_t address, const void *buffer, size_t size,
			smarter::shared_ptr<WorkQueue> wq);

	auto readSpace(uintptr_t address, void *buffer, size_t size,
			smarter::shared_ptr<WorkQueue> wq) {
		return async::transform(
			readPartialSpace(address, buffer, size, std::move(wq)),
			[=] (size_t actualSize) -> frg::expected<Error> {
				if(actualSize != size)
					return Error::fault;
				return {};
			}
		);
	}

	auto writeSpace(uintptr_t address, const void *buffer, size_t size,
			smarter::shared_ptr<WorkQueue> wq) {
		return async::transform(
			writePartialSpace(address, buffer, size, std::move(wq)),
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

	frg::expected<Error, FutexIdentity> resolveGlobalFutex(uintptr_t address) {
		// We do not take _consistencyMutex here since we are only interested in a snapshot.

		smarter::shared_ptr<Mapping> mapping;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto spaceGuard = frg::guard(&_snapshotMutex);

			mapping = _findMapping(address);
		}
		if(!mapping)
			return Error::fault;

		auto offset = address - mapping->address;
		auto [futexSpace, futexOffset] = FRG_TRY(mapping->view->resolveGlobalFutex(
				mapping->viewOffset + offset));
		return FutexIdentity{reinterpret_cast<uintptr_t>(futexSpace.get()), offset};
	}

	coroutine<frg::expected<Error, GlobalFutex>> grabGlobalFutex(uintptr_t address,
			smarter::shared_ptr<WorkQueue> wq) {
		// We do not take _consistencyMutex here since we are only interested in a snapshot.

		smarter::shared_ptr<Mapping> mapping;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto spaceGuard = frg::guard(&_snapshotMutex);

			mapping = _findMapping(address);
		}
		if(!mapping)
			co_return Error::fault;

		auto offset = address - mapping->address;
		auto [futexSpace, futexOffset] = FRG_CO_TRY(mapping->view->resolveGlobalFutex(
				mapping->viewOffset + offset));
		auto futexPhysical = FRG_CO_TRY(co_await futexSpace->takeGlobalFutex(futexOffset,
				std::move(wq)));
		co_return GlobalFutex{std::move(futexSpace), futexOffset, futexPhysical};
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
				uintptr_t offset, size_t size, PageFlags flags) override {
			return mapPresentPagesByCursor<ClientPageSpace::Cursor>(&space_->pageSpace_,
					va, view, offset, size, flags);
		}

		frg::expected<Error> remapPresentPages(VirtualAddr va, MemoryView *view,
				uintptr_t offset, size_t size, PageFlags flags) override {
			return remapPresentPagesByCursor<ClientPageSpace::Cursor>(&space_->pageSpace_,
					va, view, offset, size, flags);
		}

		frg::expected<Error> faultPage(VirtualAddr va, MemoryView *view,
				uintptr_t offset, PageFlags flags) override {
			return faultPageByCursor<ClientPageSpace::Cursor>(&space_->pageSpace_,
					va, view, offset, flags);
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
		ptr->setupInitialHole(0x100000, 0x7ffffff00000);
		return constructHandle(std::move(ptr));
	}

	static void activate(smarter::shared_ptr<AddressSpace, BindableHandle> space);

	AddressSpace();

	~AddressSpace();

	void dispose(BindableHandle);

	FutexRealm localFutexRealm;

	bool updatePageAccess(VirtualAddr address) {
		return pageSpace_.updatePageAccess(address);
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
	: _view{view}, _offset{offset}, _size{size}, _active{true} { }

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

	auto acquire(smarter::shared_ptr<WorkQueue> wq) {
		return async::transform(_view->asyncLockRange(_offset, _size, std::move(wq)),
			[&] (Error e) { _active = e == Error::success; });
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
