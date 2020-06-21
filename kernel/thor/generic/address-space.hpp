#pragma once

#include <async/basic.hpp>
#include <async/oneshot-event.hpp>
#include <frg/container_of.hpp>
#include "execution/coroutine.hpp"
#include "memory-view.hpp"

namespace thor {

struct VirtualSpace;

struct VirtualOperations {
	virtual void retire(RetireNode *node) = 0;

	virtual bool submitShootdown(ShootNode *node) = 0;

	virtual void mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
			uint32_t flags, CachingMode cachingMode) = 0;
	virtual PageStatus unmapSingle4k(VirtualAddr pointer) = 0;
	virtual PageStatus cleanSingle4k(VirtualAddr pointer) = 0;
	virtual bool isMapped(VirtualAddr pointer) = 0;

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for shootdown()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct ShootdownOperation;

	struct [[nodiscard]] ShootdownSender {
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
	struct ShootdownOperation {
		ShootdownOperation(ShootdownSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		ShootdownOperation(const ShootdownOperation &) = delete;

		ShootdownOperation &operator= (const ShootdownOperation &) = delete;

		bool start_inline() {
			worklet_.setup([] (Worklet *base) {
				auto op = frg::container_of(base, &ShootdownOperation::worklet_);
				async::execution::set_value_noinline(op->receiver_);
			});
			node_.address = s_.address;
			node_.size = s_.size;
			node_.setup(&worklet_);
			if(s_.self->submitShootdown(&node_)) {
				async::execution::set_value_inline(receiver_);
				return true;
			}
			return false;
		}

	private:
		ShootdownSender s_;
		R receiver_;
		ShootNode node_;
		Worklet worklet_;
	};

	friend async::sender_awaiter<ShootdownSender>
	operator co_await(ShootdownSender sender) {
		return {sender};
	}

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
	Mapping(size_t length, MappingFlags flags,
			frigg::SharedPtr<MemorySlice> view, uintptr_t offset);

	Mapping(const Mapping &) = delete;

	~Mapping();

	Mapping &operator= (const Mapping &) = delete;

	VirtualSpace *owner() {
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

	void tie(smarter::shared_ptr<VirtualSpace> owner, VirtualAddr address);

	void protect(MappingFlags flags);

	// Makes sure that pages are not evicted from virtual memory.
	void lockVirtualRange(uintptr_t offset, size_t length,
			async::any_receiver<frg::expected<Error>> receiver);
	void unlockVirtualRange(uintptr_t offset, size_t length);

	frg::tuple<PhysicalAddr, CachingMode>
	resolveRange(ptrdiff_t offset);

	// Ensures that a page of virtual memory is present.
	// Note that this does *not* guarantee that the page is not evicted immediately,
	// unless you hold a lock (via lockVirtualRange()).
	bool touchVirtualPage(TouchVirtualNode *node);

	// Helper function that calls touchVirtualPage() on a certain range.
	bool populateVirtualRange(PopulateVirtualNode *node);

	void install();
	void reinstall();
	void synchronize(uintptr_t offset, size_t length);
	void uninstall();
	void retire();

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
			s_.self->lockVirtualRange(s_.offset, s_.size, std::move(receiver_));
		}

	private:
		LockVirtualRangeSender s_;
		R receiver_;
	};

	friend async::sender_awaiter<LockVirtualRangeSender, frg::expected<Error>>
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

		bool start_inline() {
			worklet_.setup([] (Worklet *base) {
				auto op = frg::container_of(base, &TouchVirtualPageOperation::worklet_);
				async::execution::set_value_noinline(op->receiver_,
						frg::tuple<Error, PhysicalRange, bool>{op->node_.error(),
								op->node_.range(), op->node_.spurious()});
			});
			node_.setup(s_.offset, &worklet_);
			if(s_.self->touchVirtualPage(&node_)) {
				async::execution::set_value_inline(receiver_,
						frg::tuple<Error, PhysicalRange, bool>{node_.error(),
								node_.range(), node_.spurious()});
				return true;
			}
			return false;
		}

	private:
		TouchVirtualPageSender s_;
		R receiver_;
		TouchVirtualNode node_;
		Worklet worklet_;
	};

	friend async::sender_awaiter<TouchVirtualPageSender, frg::tuple<Error, PhysicalRange, bool>>
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

		bool start_inline() {
			worklet_.setup([] (Worklet *base) {
				auto op = frg::container_of(base, &PopulateVirtualRangeOperation::worklet_);
				async::execution::set_value_noinline(op->receiver_);
			});
			node_.setup(s_.offset, s_.size, &worklet_);
			if(s_.self->populateVirtualRange(&node_)) {
				async::execution::set_value_inline(receiver_);
				return true;
			}
			return false;
		}

	private:
		PopulateVirtualRangeSender s_;
		R receiver_;
		PopulateVirtualNode node_;
		Worklet worklet_;
	};

	friend async::sender_awaiter<PopulateVirtualRangeSender>
	operator co_await(PopulateVirtualRangeSender sender) {
		return {sender};
	}

	// ----------------------------------------------------------------------------------

	smarter::borrowed_ptr<Mapping> selfPtr;

	frg::rbtree_hook treeNode;

protected:
	uint32_t compilePageFlags();

private:
	coroutine<void> runEvictionLoop_();

	smarter::shared_ptr<VirtualSpace> _owner;
	VirtualAddr _address;
	size_t _length;
	MappingFlags _flags;

	MappingState _state = MappingState::null;
	MemoryObserver _observer;
	async::cancellation_event _cancelEviction;
	async::oneshot_event _evictionDoneEvent;
	frigg::SharedPtr<MemorySlice> _slice;
	frigg::SharedPtr<MemoryView> _view;
	size_t _viewOffset;

	frigg::TicketLock _evictMutex;
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
	friend struct VirtualSpace;
	friend struct Mapping;

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

struct AddressProtectNode {
	friend struct VirtualSpace;

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
	friend struct VirtualSpace;

	void setup(Worklet *completion) {
		_completion = completion;
	}

	void complete() {
		WorkQueue::post(_completion);
	}

private:
	Worklet *_completion;
	VirtualSpace *_space;
	smarter::shared_ptr<Mapping> _mapping;
	Worklet _worklet;
	ShootNode _shootNode;
};

struct VirtualSpace {
	friend struct AddressSpaceLockHandle;
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
	};

	enum FaultFlags : uint32_t {
		kFaultWrite = (1 << 1),
		kFaultExecute = (1 << 2)
	};

	VirtualSpace(VirtualOperations *ops);

	~VirtualSpace();

	void retire();

	smarter::shared_ptr<Mapping> getMapping(VirtualAddr address);

	void setupInitialHole(VirtualAddr address, size_t size);

	Error map(frigg::UnsafePtr<MemorySlice> view,
			VirtualAddr address, size_t offset, size_t length,
			uint32_t flags, VirtualAddr *actual_address);

	bool protect(VirtualAddr address, size_t length, uint32_t flags, AddressProtectNode *node);

	void synchronize(VirtualAddr address, size_t length,
			async::any_receiver<void> receiver);

	bool unmap(VirtualAddr address, size_t length, AddressUnmapNode *node);

	bool handleFault(VirtualAddr address, uint32_t flags, FaultNode *node);

	size_t rss() {
		return _residuentSize;
	}

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for synchronize()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct SynchronizeOperation;

	struct [[nodiscard]] SynchronizeSender {
		template<typename R>
		friend SynchronizeOperation<R>
		connect(SynchronizeSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		VirtualSpace *self;
		VirtualAddr address;
		size_t size;
	};

	SynchronizeSender synchronize(VirtualAddr address, size_t size) {
		return {this, address, size};
	}

	template<typename R>
	struct SynchronizeOperation {
		SynchronizeOperation(SynchronizeSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		SynchronizeOperation(const SynchronizeOperation &) = delete;

		SynchronizeOperation &operator= (const SynchronizeOperation &) = delete;

		void start() {
			s_.self->synchronize(s_.address, s_.size, std::move(receiver_));
		}

	private:
		SynchronizeSender s_;
		R receiver_;
	};

	friend async::sender_awaiter<SynchronizeSender>
	operator co_await(SynchronizeSender sender) {
		return {sender};
	}

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for unmap()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct UnmapOperation;

	struct [[nodiscard]] UnmapSender {
		template<typename R>
		friend UnmapOperation<R>
		connect(UnmapSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		VirtualSpace *self;
		VirtualAddr address;
		size_t size;
	};

	UnmapSender unmap(VirtualAddr address, size_t size) {
		return {this, address, size};
	}

	template<typename R>
	struct UnmapOperation {
		UnmapOperation(UnmapSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		UnmapOperation(const UnmapOperation &) = delete;

		UnmapOperation &operator= (const UnmapOperation &) = delete;

		bool start_inline() {
			worklet_.setup([] (Worklet *base) {
				auto op = frg::container_of(base, &UnmapOperation::worklet_);
				async::execution::set_value_noinline(op->receiver_);
			});
			node_.setup(&worklet_);
			if(s_.self->unmap(s_.address, s_.size, &node_)) {
				async::execution::set_value_inline(receiver_);
				return true;
			}
			return false;
		}

	private:
		UnmapSender s_;
		R receiver_;
		AddressUnmapNode node_;
		Worklet worklet_;
	};

	friend async::sender_awaiter<UnmapSender>
	operator co_await(UnmapSender sender) {
		return {sender};
	}

	// ----------------------------------------------------------------------------------

	smarter::borrowed_ptr<VirtualSpace> selfPtr;

private:
	// Allocates a new mapping of the given length somewhere in the address space.
	VirtualAddr _allocate(size_t length, MapFlags flags);

	VirtualAddr _allocateAt(VirtualAddr address, size_t length);

	smarter::shared_ptr<Mapping> _findMapping(VirtualAddr address);

	// Splits some memory range from a hole mapping.
	void _splitHole(Hole *hole, VirtualAddr offset, VirtualAddr length);

	VirtualOperations *_ops;

	frigg::TicketLock _mutex;
	HoleTree _holes;
	MappingTree _mappings;

	int64_t _residuentSize = 0;
};

struct AddressSpace : VirtualSpace, smarter::crtp_counter<AddressSpace, BindableHandle> {
	friend struct AddressSpaceLockHandle;
	friend struct Mapping;

	// Silence Clang warning about hidden overloads.
	using smarter::crtp_counter<AddressSpace, BindableHandle>::dispose;

	struct Operations : VirtualOperations {
		Operations(AddressSpace *space)
		: space_{space} { }

		void retire(RetireNode *node) override {
			return space_->pageSpace_.retire(node);
		}

		bool submitShootdown(ShootNode *node) override {
			return space_->pageSpace_.submitShootdown(node);
		}

		void mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
				uint32_t flags, CachingMode cachingMode) override {
			space_->pageSpace_.mapSingle4k(pointer, physical, true, flags, cachingMode);
		}

		PageStatus unmapSingle4k(VirtualAddr pointer) override {
			return space_->pageSpace_.unmapSingle4k(pointer);
		}

		PageStatus cleanSingle4k(VirtualAddr pointer) override {
			return space_->pageSpace_.cleanSingle4k(pointer);
		}

		bool isMapped(VirtualAddr pointer) override {
			return space_->pageSpace_.isMapped(pointer);
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

	Futex futexSpace;

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

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for acquire()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct [[nodiscard]] AcquireOperation {
		AcquireOperation(AddressSpaceLockHandle *handle, R receiver)
		: handle_{handle}, receiver_{std::move(receiver)} { }

		AcquireOperation(const AcquireOperation &) = delete;

		AcquireOperation &operator= (const AcquireOperation &) = delete;

		bool start_inline() {
			worklet_.setup([] (Worklet *base) {
				auto op = frg::container_of(base, &AcquireOperation::worklet_);
				async::execution::set_value_noinline(op->receiver_);
			});
			node_.setup(&worklet_);
			if(handle_->acquire(&node_)) {
				async::execution::set_value_inline(receiver_);
				return true;
			}
			return false;
		}

	private:
		AddressSpaceLockHandle *handle_;
		R receiver_;
		AcquireNode node_;
		Worklet worklet_;
	};

	struct [[nodiscard]] AcquireSender {
		template<typename R>
		AcquireOperation<R> connect(R receiver) {
			return {handle, std::move(receiver)};
		}

		async::sender_awaiter<AcquireSender> operator co_await() {
			return {*this};
		}

		AddressSpaceLockHandle *handle;
	};

	AcquireSender acquire() {
		return {this};
	}

	// ----------------------------------------------------------------------------------

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
