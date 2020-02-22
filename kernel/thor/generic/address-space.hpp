#pragma once

#include <frg/container_of.hpp>
#include "execution/basics.hpp"
#include "memory-view.hpp"

namespace thor {

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

	forkMask = 0x07,
	dropAtFork = 0x01,
	shareAtFork = 0x02,
	copyOnWriteAtFork = 0x04,

	permissionMask = 0x70,
	protRead = 0x10,
	protWrite = 0x20,
	protExecute = 0x40,

	dontRequireBacking = 0x100
};

struct LockVirtualNode {
	static void post(LockVirtualNode *node) {
		WorkQueue::post(node->_worklet);
	}

	void setup(uintptr_t offset, size_t size, Worklet *worklet) {
		_offset = offset;
		_size = size;
		_worklet = worklet;
	}

	auto offset() { return _offset; }
	auto size() { return _size; }

private:
	uintptr_t _offset;
	size_t _size;
	Worklet *_worklet;
};

struct TouchVirtualNode {
	void setup(uintptr_t offset, Worklet *worklet) {
		_offset = offset;
		_worklet = worklet;
	}

	void setResult(Error error) {
		_error = error;
	}
	void setResult(Error error, PhysicalRange range, bool spurious = false) {
		_error = error;
		_range = range;
		_spurious = spurious;
	}
	void setResult(Error error, PhysicalAddr physical, size_t size, CachingMode mode,
			bool spurious = false) {
		_error = error;
		_range = PhysicalRange{physical, size, mode};
		_spurious = spurious;
	}

	Error error() {
		return _error;
	}

	PhysicalRange range() {
		return _range;
	}

	bool spurious() {
		return _spurious;
	}

	uintptr_t _offset;
	Worklet *_worklet;

private:
	Error _error;
	PhysicalRange _range;
	bool _spurious;
};

struct PopulateVirtualNode {
	void setup(uintptr_t offset, size_t size, Worklet *prepared) {
		_offset = offset;
		_size = size;
		_prepared = prepared;
	}

	uintptr_t _offset;
	size_t _size;
	Worklet *_prepared;
};

enum class MappingState {
	null,
	active,
	zombie,
	retired
};

struct Mapping {
	Mapping(size_t length, MappingFlags flags);

	Mapping(const Mapping &) = delete;

	virtual ~Mapping() = default;

	Mapping &operator= (const Mapping &) = delete;

	AddressSpace *owner() {
		return _owner.get();
	}

	VirtualAddr address() const {
		return _address;
	}

	size_t length() const {
		return _length;
	}

	MappingFlags flags() const {
		return _flags;
	}

public:
	void tie(smarter::shared_ptr<AddressSpace> owner, VirtualAddr address);

	void protect(MappingFlags flags);

	// Makes sure that pages are not evicted from virtual memory.
	virtual bool lockVirtualRange(LockVirtualNode *node) = 0;
	virtual void unlockVirtualRange(uintptr_t offset, size_t length) = 0;

	virtual frg::tuple<PhysicalAddr, CachingMode>
	resolveRange(ptrdiff_t offset) = 0;

	// Ensures that a page of virtual memory is present.
	// Note that this does *not* guarantee that the page is not evicted immediately,
	// unless you hold a lock (via lockVirtualRange()).
	virtual bool touchVirtualPage(TouchVirtualNode *node) = 0;

	// Helper function that calls touchVirtualPage() on a certain range.
	bool populateVirtualRange(PopulateVirtualNode *node);

	virtual smarter::shared_ptr<Mapping> forkMapping() = 0;

	virtual void install() = 0;
	virtual void reinstall() = 0;
	virtual void uninstall() = 0;
	virtual void retire() = 0;

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for lockVirtualRange()
	// ----------------------------------------------------------------------------------

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
	};

	LockVirtualRangeSender lockVirtualRange(uintptr_t offset, size_t size) {
		return {this, offset, size};
	}

	template<typename R>
	struct LockVirtualRangeOperation {
		LockVirtualRangeOperation(LockVirtualRangeSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		LockVirtualRangeOperation(const LockVirtualRangeOperation &) = delete;

		LockVirtualRangeOperation &operator= (const LockVirtualRangeOperation &) = delete;

		void start() {
			worklet_.setup([] (Worklet *base) {
				auto op = frg::container_of(base, &LockVirtualRangeOperation::worklet_);
				op->receiver_.set_done();
			});
			node_.setup(s_.offset, s_.size, &worklet_);
			if(s_.self->lockVirtualRange(&node_))
				WorkQueue::post(&worklet_); // Force into slow path for now.
		}

	private:
		LockVirtualRangeSender s_;
		R receiver_;
		LockVirtualNode node_;
		Worklet worklet_;
	};

	friend execution::sender_awaiter<LockVirtualRangeSender>
	operator co_await(LockVirtualRangeSender sender) {
		return {sender};
	}

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for touchVirtualPage()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct TouchVirtualPageOperation;

	struct [[nodiscard]] TouchVirtualPageSender {
		template<typename R>
		friend TouchVirtualPageOperation<R>
		connect(TouchVirtualPageSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		Mapping *self;
		uintptr_t offset;
	};

	TouchVirtualPageSender touchVirtualPage(uintptr_t offset) {
		return {this, offset};
	}

	template<typename R>
	struct TouchVirtualPageOperation {
		TouchVirtualPageOperation(TouchVirtualPageSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		TouchVirtualPageOperation(const TouchVirtualPageOperation &) = delete;

		TouchVirtualPageOperation &operator= (const TouchVirtualPageOperation &) = delete;

		void start() {
			worklet_.setup([] (Worklet *base) {
				auto op = frg::container_of(base, &TouchVirtualPageOperation::worklet_);
				op->receiver_.set_done({op->node_.error(), op->node_.range(), op->node_.spurious()});
			});
			node_.setup(s_.offset, &worklet_);
			if(s_.self->touchVirtualPage(&node_))
				WorkQueue::post(&worklet_); // Force into slow path for now.
		}

	private:
		TouchVirtualPageSender s_;
		R receiver_;
		TouchVirtualNode node_;
		Worklet worklet_;
	};

	friend execution::sender_awaiter<TouchVirtualPageSender, frg::tuple<Error, PhysicalRange, bool>>
	operator co_await(TouchVirtualPageSender sender) {
		return {sender};
	}

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for populateVirtualRange()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct PopulateVirtualRangeOperation;

	struct [[nodiscard]] PopulateVirtualRangeSender {
		template<typename R>
		friend PopulateVirtualRangeOperation<R>
		connect(PopulateVirtualRangeSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		Mapping *self;
		uintptr_t offset;
		size_t size;
	};

	PopulateVirtualRangeSender populateVirtualRange(uintptr_t offset, size_t size) {
		return {this, offset, size};
	}

	template<typename R>
	struct PopulateVirtualRangeOperation {
		PopulateVirtualRangeOperation(PopulateVirtualRangeSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		PopulateVirtualRangeOperation(const PopulateVirtualRangeOperation &) = delete;

		PopulateVirtualRangeOperation &operator= (const PopulateVirtualRangeOperation &) = delete;

		void start() {
			worklet_.setup([] (Worklet *base) {
				auto op = frg::container_of(base, &PopulateVirtualRangeOperation::worklet_);
				op->receiver_.set_done();
			});
			node_.setup(s_.offset, s_.size, &worklet_);
			if(s_.self->populateVirtualRange(&node_))
				WorkQueue::post(&worklet_); // Force into slow path for now.
		}

	private:
		PopulateVirtualRangeSender s_;
		R receiver_;
		PopulateVirtualNode node_;
		Worklet worklet_;
	};

	friend execution::sender_awaiter<PopulateVirtualRangeSender>
	operator co_await(PopulateVirtualRangeSender sender) {
		return {sender};
	}

	// ----------------------------------------------------------------------------------

	smarter::borrowed_ptr<Mapping> selfPtr;

	frg::rbtree_hook treeNode;

protected:
	uint32_t compilePageFlags();

private:
	smarter::shared_ptr<AddressSpace> _owner;
	VirtualAddr _address;
	size_t _length;
	MappingFlags _flags;
};

struct NormalMapping : Mapping, MemoryObserver {
	friend struct AddressSpace;

	NormalMapping(size_t length, MappingFlags flags,
			frigg::SharedPtr<MemorySlice> view, uintptr_t offset);

	~NormalMapping();

	bool lockVirtualRange(LockVirtualNode *node) override;
	void unlockVirtualRange(uintptr_t offset, size_t length) override;
	frg::tuple<PhysicalAddr, CachingMode> resolveRange(ptrdiff_t offset) override;
	bool touchVirtualPage(TouchVirtualNode *node) override;

	smarter::shared_ptr<Mapping> forkMapping() override;

	void install() override;
	void reinstall() override;
	void uninstall() override;
	void retire() override;

	bool observeEviction(uintptr_t offset, size_t length, EvictNode *node) override;

private:
	MappingState _state = MappingState::null;
	frigg::SharedPtr<MemorySlice> _slice;
	frigg::SharedPtr<MemoryView> _view;
	size_t _viewOffset;
};

struct CowChain {
	CowChain(frigg::SharedPtr<CowChain> chain);

	~CowChain();

// TODO: Either this private again or make this class POD-like.
	frigg::TicketLock _mutex;

	frigg::SharedPtr<CowChain> _superChain;
	frg::rcu_radixtree<std::atomic<PhysicalAddr>, KernelAlloc> _pages;
};

struct CowMapping : Mapping, MemoryObserver {
	friend struct AddressSpace;

	CowMapping(size_t length, MappingFlags flags,
			frigg::SharedPtr<MemorySlice> slice, uintptr_t view_offset,
			frigg::SharedPtr<CowChain> chain);

	~CowMapping();

	bool lockVirtualRange(LockVirtualNode *node) override;
	void unlockVirtualRange(uintptr_t offset, size_t length) override;
	frg::tuple<PhysicalAddr, CachingMode> resolveRange(ptrdiff_t offset) override;
	bool touchVirtualPage(TouchVirtualNode *node) override;

	smarter::shared_ptr<Mapping> forkMapping() override;

	void install() override;
	void reinstall() override;
	void uninstall() override;
	void retire() override;

	bool observeEviction(uintptr_t offset, size_t length, EvictNode *node) override;

private:
	enum class CowState {
		null,
		inProgress,
		hasCopy
	};

	struct CowPage {
		PhysicalAddr physical = -1;
		CowState state = CowState::null;
		unsigned int lockCount = 0;
	};

	frigg::TicketLock _mutex;

	frigg::SharedPtr<MemorySlice> _slice;
	uintptr_t _viewOffset;
	frigg::SharedPtr<CowChain> _copyChain;

	MappingState _state = MappingState::null;
	frg::rcu_radixtree<CowPage, KernelAlloc> _ownedPages;
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
		return a.address() < b.address();
	}
};

using MappingTree = frg::rbtree<
	Mapping,
	&Mapping::treeNode,
	MappingLess
>;

struct FaultNode {
	friend struct AddressSpace;
	friend struct NormalMapping;
	friend struct CowMapping;

	FaultNode()
	: _resolved{false} { }

	FaultNode(const FaultNode &) = delete;

	FaultNode &operator= (const FaultNode &) = delete;

	void setup(Worklet *handled) {
		_handled = handled;
	}

	bool resolved() {
		return _resolved;
	}

private:
	VirtualAddr _address;
	uint32_t _flags;
	Worklet *_handled;

	bool _resolved;

	smarter::shared_ptr<Mapping> _mapping;
	Worklet _worklet;
	TouchVirtualNode _touchVirtual;
};

struct ForkItem {
	Mapping *mapping;
};

struct ForkNode {
	friend struct AddressSpace;

	ForkNode()
	: _items{*kernelAlloc} { }

	void setup(Worklet *forked) {
		_forked = forked;
	}
	smarter::shared_ptr<AddressSpace, BindableHandle> forkedSpace() {
		return frigg::move(_fork);
	}

private:
	Worklet *_forked;

	// TODO: This should be a SharedPtr, too.
	AddressSpace *_original;
	smarter::shared_ptr<AddressSpace, BindableHandle> _fork;
	frigg::LinkedList<ForkItem, KernelAlloc> _items;
	Worklet _worklet;
	ShootNode _shootNode;
};

struct AddressProtectNode {
	friend struct AddressSpace;

	void setup(Worklet *completion) {
		_completion = completion;
	}

	void complete() {
		WorkQueue::post(_completion);
	}

private:
	Worklet *_completion;
	Worklet _worklet;
	ShootNode _shootNode;
};

struct AddressUnmapNode {
	friend struct AddressSpace;

	void setup(Worklet *completion) {
		_completion = completion;
	}

	void complete() {
		WorkQueue::post(_completion);
	}

private:
	Worklet *_completion;
	AddressSpace *_space;
	smarter::shared_ptr<Mapping> _mapping;
	Worklet _worklet;
	ShootNode _shootNode;
};

struct AddressSpace : smarter::crtp_counter<AddressSpace, BindableHandle> {
	friend struct AddressSpaceLockHandle;
	friend struct NormalMapping;
	friend struct CowMapping;

	// Silence Clang warning about hidden overloads.
	using smarter::crtp_counter<AddressSpace, BindableHandle>::dispose;

public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	typedef uint32_t MapFlags;
	enum : MapFlags {
		kMapShareAtFork = 0x80,
		kMapCopyOnWrite = 0x800,

		kMapFixed = 0x01,
		kMapPreferBottom = 0x02,
		kMapPreferTop = 0x04,
		kMapProtRead = 0x08,
		kMapProtWrite = 0x10,
		kMapProtExecute = 0x20,
		kMapDropAtFork = 0x40,
		kMapCopyOnWriteAtFork = 0x100,
		kMapPopulate = 0x200,
		kMapDontRequireBacking = 0x400,
	};

	enum FaultFlags : uint32_t {
		kFaultWrite = (1 << 1),
		kFaultExecute = (1 << 2)
	};

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
		return constructHandle(std::move(ptr));
	}

	static void activate(smarter::shared_ptr<AddressSpace, BindableHandle> space);

	AddressSpace();

	~AddressSpace();

	void dispose(BindableHandle);

	void setupDefaultMappings();

	smarter::shared_ptr<Mapping> getMapping(VirtualAddr address);

	Error map(Guard &guard, frigg::UnsafePtr<MemorySlice> view,
			VirtualAddr address, size_t offset, size_t length,
			uint32_t flags, VirtualAddr *actual_address);

	bool protect(VirtualAddr address, size_t length, uint32_t flags, AddressProtectNode *node);

	bool unmap(VirtualAddr address, size_t length, AddressUnmapNode *node);

	bool handleFault(VirtualAddr address, uint32_t flags, FaultNode *node);

	bool fork(ForkNode *node);

	size_t rss() {
		return _residuentSize;
	}

	Lock lock;

	Futex futexSpace;

private:

	// Allocates a new mapping of the given length somewhere in the address space.
	VirtualAddr _allocate(size_t length, MapFlags flags);

	VirtualAddr _allocateAt(VirtualAddr address, size_t length);

	smarter::shared_ptr<Mapping> _findMapping(VirtualAddr address);

	// Splits some memory range from a hole mapping.
	void _splitHole(Hole *hole, VirtualAddr offset, VirtualAddr length);

	smarter::borrowed_ptr<AddressSpace> selfPtr;

	HoleTree _holes;
	MappingTree _mappings;

	ClientPageSpace _pageSpace;

	int64_t _residuentSize = 0;
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

	MemoryViewLockHandle(frigg::SharedPtr<MemoryView> view, uintptr_t offset, size_t size);

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

private:
	frigg::SharedPtr<MemoryView> _view = nullptr;
	uintptr_t _offset = 0;
	size_t _size = 0;
	bool _active = false;
};

struct AcquireNode {
	friend struct AddressSpaceLockHandle;

	AcquireNode()
	: _acquired{nullptr} { }

	AcquireNode(const AcquireNode &) = delete;

	AcquireNode &operator= (const AcquireNode &) = delete;

	void setup(Worklet *acquire) {
		_acquired = acquire;
	}

private:
	Worklet *_acquired;
};

struct AddressSpaceLockHandle {
public:
	friend void swap(AddressSpaceLockHandle &a, AddressSpaceLockHandle &b) {
		using std::swap;
		swap(a._space, b._space);
		swap(a._mapping, b._mapping);
		swap(a._address, b._address);
		swap(a._length, b._length);
		swap(a._active, b._active);
	}

	AddressSpaceLockHandle() = default;

	AddressSpaceLockHandle(smarter::shared_ptr<AddressSpace, BindableHandle> space,
			void *pointer, size_t length);

	AddressSpaceLockHandle(const AddressSpaceLockHandle &other) = delete;

	AddressSpaceLockHandle(AddressSpaceLockHandle &&other)
	: AddressSpaceLockHandle() {
		swap(*this, other);
	}

	~AddressSpaceLockHandle();

	AddressSpaceLockHandle &operator= (AddressSpaceLockHandle other) {
		swap(*this, other);
		return *this;
	}

	explicit operator bool () {
		return _active;
	}

	smarter::borrowed_ptr<AddressSpace, BindableHandle> space() {
		return _space;
	}
	uintptr_t address() {
		return _address;
	}
	size_t length() {
		return _length;
	}

	bool acquire(AcquireNode *node);

	PhysicalAddr getPhysical(size_t offset);

	void load(size_t offset, void *pointer, size_t size);
	Error write(size_t offset, const void *pointer, size_t size);

	template<typename T>
	T read(size_t offset) {
		T value;
		load(offset, &value, sizeof(T));
		return value;
	}

	template<typename T>
	Error write(size_t offset, T value) {
		return write(offset, &value, sizeof(T));
	}

private:
	PhysicalAddr _resolvePhysical(VirtualAddr vaddr);

	smarter::shared_ptr<AddressSpace, BindableHandle> _space;
	smarter::shared_ptr<Mapping> _mapping;
	uintptr_t _address = 0;
	size_t _length = 0;
	bool _active = false; // Whether the accessor is acquired successfully.
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
