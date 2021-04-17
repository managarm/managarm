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

struct VirtualOperations {
	virtual void retire(RetireNode *node) = 0;

	virtual bool submitShootdown(ShootNode *node) = 0;

	virtual void mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
			uint32_t flags, CachingMode cachingMode) = 0;
	virtual PageStatus unmapSingle4k(VirtualAddr pointer) = 0;
	virtual PageStatus cleanSingle4k(VirtualAddr pointer) = 0;
	virtual bool isMapped(VirtualAddr pointer) = 0;

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for retire()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct RetireOperation : private RetireNode {
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
	struct ShootdownOperation : private ShootNode {
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
	struct LockVirtualRangeOperation : private LockVirtualRangeNode {
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
	// Sender boilerplate for touchVirtualPage()
	// ----------------------------------------------------------------------------------
private:
	struct TouchVirtualPageNode {
		virtual void resume() = 0;

		frg::expected<Error, TouchVirtualResult> result;
	};

	// Ensures that a page of virtual memory is present.
	// Note that this does *not* guarantee that the page is not evicted immediately,
	// unless you hold a lock (via lockVirtualRange()).
	void touchVirtualPage(uintptr_t offset,
			smarter::shared_ptr<WorkQueue> wq, TouchVirtualPageNode *node);

public:
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
		smarter::shared_ptr<WorkQueue> wq;
	};

	TouchVirtualPageSender touchVirtualPage(uintptr_t offset,
			smarter::shared_ptr<WorkQueue> wq) {
		return {this, offset, std::move(wq)};
	}

	template<typename R>
	struct TouchVirtualPageOperation : private TouchVirtualPageNode {
		TouchVirtualPageOperation(TouchVirtualPageSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		TouchVirtualPageOperation(const TouchVirtualPageOperation &) = delete;

		TouchVirtualPageOperation &operator= (const TouchVirtualPageOperation &) = delete;

		void start() {
			// XXX: work around Clang bug that runs s_.wq dtor after the call.
			auto wq = s_.wq;
			s_.self->touchVirtualPage(s_.offset, std::move(wq), this);
		}

	private:
		void resume() override {
			async::execution::set_value(receiver_, result);
		}

		TouchVirtualPageSender s_;
		R receiver_;
	};

	friend async::sender_awaiter<TouchVirtualPageSender, frg::expected<Error, TouchVirtualResult>>
	operator co_await(TouchVirtualPageSender sender) {
		return {sender};
	}

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for populateVirtualRange()
	// ----------------------------------------------------------------------------------
private:
	struct PopulateVirtualRangeNode {
		virtual void resume() = 0;

		frg::expected<Error> result;
	};

	// Helper function that calls touchVirtualPage() on a certain range.
	void populateVirtualRange(uintptr_t offset, size_t size,
			smarter::shared_ptr<WorkQueue> wq, PopulateVirtualRangeNode *node);

public:
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
		smarter::shared_ptr<WorkQueue> wq;
	};

	PopulateVirtualRangeSender populateVirtualRange(uintptr_t offset, size_t size,
			smarter::shared_ptr<WorkQueue> wq) {
		return {this, offset, size, std::move(wq)};
	}

	template<typename R>
	struct PopulateVirtualRangeOperation : private PopulateVirtualRangeNode {
		PopulateVirtualRangeOperation(PopulateVirtualRangeSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		PopulateVirtualRangeOperation(const PopulateVirtualRangeOperation &) = delete;

		PopulateVirtualRangeOperation &operator= (const PopulateVirtualRangeOperation &) = delete;

		void start() {
			// XXX: work around Clang bug that runs s_.wq dtor after the call.
			auto wq = s_.wq;
			s_.self->populateVirtualRange(s_.offset, s_.size, std::move(wq), this);
		}

	private:
		void resume() override {
			async::execution::set_value(receiver_, result);
		}

		PopulateVirtualRangeSender s_;
		R receiver_;
	};

	friend async::sender_awaiter<PopulateVirtualRangeSender, frg::expected<Error>>
	operator co_await(PopulateVirtualRangeSender sender) {
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

struct MapNode {
	friend struct VirtualSpace;

	MapNode() = default;

	MapNode(const MapNode &) = delete;

	MapNode &operator= (const MapNode &) = delete;

	frg::expected<Error, VirtualAddr> result() {
		return *nodeResult_;
	}

protected:
	virtual void resume() = 0;

private:
	frg::optional<frg::expected<Error, VirtualAddr>> nodeResult_;
};

struct SynchronizeNode {
	friend struct VirtualSpace;

	SynchronizeNode() = default;

	SynchronizeNode(const SynchronizeNode &) = delete;

	SynchronizeNode &operator= (const SynchronizeNode &) = delete;

protected:
	virtual void resume() = 0;
};

struct FaultNode {
	friend struct VirtualSpace;

	FaultNode() = default;

	FaultNode(const FaultNode &) = delete;

	FaultNode &operator= (const FaultNode &) = delete;

protected:
	virtual void complete(bool resolved) = 0;
};

struct AddressProtectNode {
	friend struct VirtualSpace;

protected:
	virtual void complete() = 0;
};

struct AddressUnmapNode {
	friend struct VirtualSpace;

protected:
	virtual void complete() = 0;
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

	bool map(smarter::borrowed_ptr<MemorySlice> view,
			VirtualAddr address, size_t offset, size_t length, uint32_t flags,
			MapNode *node);

	bool protect(VirtualAddr address, size_t length, uint32_t flags, AddressProtectNode *node);

	void synchronize(VirtualAddr address, size_t length, SynchronizeNode *node);

	bool unmap(VirtualAddr address, size_t length, AddressUnmapNode *node);

	frg::optional<bool> handleFault(VirtualAddr address, uint32_t flags,
			smarter::shared_ptr<WorkQueue> wq, FaultNode *node);

	size_t rss() {
		return _residuentSize;
	}

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for map()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct [[nodiscard]] MapOperation : private MapNode {
		MapOperation(VirtualSpace *self, smarter::borrowed_ptr<MemorySlice> slice,
				VirtualAddr address, size_t offset, size_t length, uint32_t flags,
				R receiver)
		: self_{self}, slice_{slice},
				address_{address}, offset_{offset}, length_{length}, flags_{flags},
				receiver_{std::move(receiver)} { }

		MapOperation(const MapOperation &) = delete;

		MapOperation &operator= (const MapOperation &) = delete;

		bool start_inline() {
			if(self_->map(slice_, address_, offset_, length_, flags_, this)) {
				async::execution::set_value_inline(receiver_, result());
				return true;
			}
			return false;
		}

	private:
		void resume() override {
			async::execution::set_value_noinline(receiver_, result());
		}

		VirtualSpace *self_;
		smarter::borrowed_ptr<MemorySlice> slice_;
		VirtualAddr address_;
		size_t offset_;
		size_t length_;
		uint32_t flags_;
		R receiver_;
	};

	struct [[nodiscard]] MapSender {
		using value_type = frg::expected<Error, VirtualAddr>;

		async::sender_awaiter<MapSender, frg::expected<Error, VirtualAddr>>
		operator co_await() {
			return {*this};
		}

		template<typename R>
		MapOperation<R> connect(R receiver) {
			return {self, slice, address, offset, length, flags, std::move(receiver)};
		}

		VirtualSpace *self;
		smarter::borrowed_ptr<MemorySlice> slice;
		VirtualAddr address;
		size_t offset;
		size_t length;
		uint32_t flags;
	};

	MapSender map(smarter::borrowed_ptr<MemorySlice> slice,
			VirtualAddr address, size_t offset, size_t length, uint32_t flags) {
		return {this, slice, address, offset, length, flags};
	}

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for synchronize()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct SynchronizeOperation : private SynchronizeNode {
		SynchronizeOperation(VirtualSpace *self, VirtualAddr address, size_t size, R receiver)
		: self_{self}, address_{address}, size_{size}, receiver_{std::move(receiver)} { }

		SynchronizeOperation(const SynchronizeOperation &) = delete;

		SynchronizeOperation &operator= (const SynchronizeOperation &) = delete;

		void start() {
			self_->synchronize(address_, size_, this);
		}

	private:
		void resume() override {
			async::execution::set_value(receiver_);
		}

		VirtualSpace *self_;
		VirtualAddr address_;
		size_t size_;
		R receiver_;
	};

	struct [[nodiscard]] SynchronizeSender {
		async::sender_awaiter<SynchronizeSender>
		operator co_await() {
			return {*this};
		}

		template<typename R>
		SynchronizeOperation<R> connect(R receiver) {
			return {self, address, size, std::move(receiver)};
		}

		VirtualSpace *self;
		VirtualAddr address;
		size_t size;
	};

	SynchronizeSender synchronize(VirtualAddr address, size_t size) {
		return {this, address, size};
	}

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for unmap()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct UnmapOperation;

	struct [[nodiscard]] UnmapSender {
		using value_type = void;

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
	struct UnmapOperation : private AddressUnmapNode {
		UnmapOperation(UnmapSender s, R receiver)
		: s_{s}, receiver_{std::move(receiver)} { }

		UnmapOperation(const UnmapOperation &) = delete;

		UnmapOperation &operator= (const UnmapOperation &) = delete;

		bool start_inline() {
			if(s_.self->unmap(s_.address, s_.size, this)) {
				async::execution::set_value_inline(receiver_);
				return true;
			}
			return false;
		}

	private:
		void complete() override {
			async::execution::set_value_noinline(receiver_);
		}

		UnmapSender s_;
		R receiver_;
	};

	friend async::sender_awaiter<UnmapSender>
	operator co_await(UnmapSender sender) {
		return {sender};
	}

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for protect()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct ProtectOperation;

	struct [[nodiscard]] ProtectSender {
		using value_type = void;

		template<typename R>
		friend ProtectOperation<R>
		connect(ProtectSender sender, R receiver) {
			return {sender, std::move(receiver)};
		}

		VirtualSpace *self;
		VirtualAddr address;
		size_t size;
		uint32_t flags;
	};

	ProtectSender protect(VirtualAddr address, size_t size, uint32_t flags) {
		return {this, address, size, flags};
	}

	template<typename R>
	struct ProtectOperation : private AddressProtectNode {
		ProtectOperation(ProtectSender s, R receiver)
		: self_{s.self}, address_{s.address}, size_{s.size},
				flags_{s.flags}, receiver_{std::move(receiver)} { }

		ProtectOperation(const ProtectOperation &) = delete;

		ProtectOperation &operator= (const ProtectOperation &) = delete;

		bool start_inline() {
			if(self_->protect(address_, size_, flags_, this)) {
				async::execution::set_value_inline(receiver_);
				return true;
			}
			return false;
		}

	private:
		void complete() override {
			async::execution::set_value_noinline(receiver_);
		}

		VirtualSpace *self_;
		VirtualAddr address_;
		size_t size_;
		uint32_t flags_;
		R receiver_;
	};

	friend async::sender_awaiter<ProtectSender>
	operator co_await(ProtectSender sender) {
		return {sender};
	}

	// ----------------------------------------------------------------------------------
	// Sender boilerplate for handleFault()
	// ----------------------------------------------------------------------------------

	template<typename R>
	struct HandleFaultOperation : private FaultNode {
		HandleFaultOperation(VirtualSpace *self, VirtualAddr address, uint32_t flags,
				smarter::shared_ptr<WorkQueue> wq, R receiver)
		: self_{self}, address_{address}, flags_{flags},
				wq_{std::move(wq)}, receiver_{std::move(receiver)} { }

		HandleFaultOperation(const HandleFaultOperation &) = delete;

		HandleFaultOperation &operator= (const HandleFaultOperation &) = delete;

		bool start_inline() {
			auto result = self_->handleFault(address_, flags_, std::move(wq_), this);
			if(result) {
				async::execution::set_value_inline(receiver_, *result);
				return true;
			}
			return false;
		}

	private:
		void complete(bool resolved) {
			async::execution::set_value_noinline(receiver_, resolved);
		}

		VirtualSpace *self_;
		VirtualAddr address_;
		uint32_t flags_;
		smarter::shared_ptr<WorkQueue> wq_;
		R receiver_;
	};

	struct [[nodiscard]] HandleFaultSender {
		using value_type = bool;

		template<typename R>
		HandleFaultOperation<R> connect(R receiver) {
			return {self, address, flags, std::move(wq), std::move(receiver)};
		}

		async::sender_awaiter<HandleFaultSender, bool> operator co_await() {
			return {*this};
		}

		VirtualSpace *self;
		VirtualAddr address;
		uint32_t flags;
		smarter::shared_ptr<WorkQueue> wq;
	};

	HandleFaultSender handleFault(VirtualAddr address, uint32_t flags,
			smarter::shared_ptr<WorkQueue> wq) {
		return {this, address, flags, std::move(wq)};
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

	frg::ticket_spinlock _mutex;
	HoleTree _holes;
	MappingTree _mappings;

	int64_t _residuentSize = 0;
};

coroutine<frg::expected<Error>> readVirtualSpace(VirtualSpace *space,
		uintptr_t address, void *buffer, size_t size,
		smarter::shared_ptr<WorkQueue> wq);
coroutine<frg::expected<Error>> writeVirtualSpace(VirtualSpace *space,
		uintptr_t address, const void *buffer, size_t size,
		smarter::shared_ptr<WorkQueue> wq);

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

struct AcquireNode {
	friend struct AddressSpaceLockHandle;

	AcquireNode() = default;

	AcquireNode(const AcquireNode &) = delete;

	AcquireNode &operator= (const AcquireNode &) = delete;

protected:
	virtual void complete() = 0;
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

	bool acquire(smarter::shared_ptr<WorkQueue> wq, AcquireNode *node);

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
	struct [[nodiscard]] AcquireOperation : private AcquireNode {
		AcquireOperation(AddressSpaceLockHandle *handle,
				smarter::shared_ptr<WorkQueue> wq, R receiver)
		: handle_{handle}, wq_{std::move(wq)}, receiver_{std::move(receiver)} { }

		AcquireOperation(const AcquireOperation &) = delete;

		AcquireOperation &operator= (const AcquireOperation &) = delete;

		bool start_inline() {
			if(handle_->acquire(wq_, this)) {
				async::execution::set_value_inline(receiver_);
				return true;
			}
			return false;
		}

	private:
		void complete() override {
			async::execution::set_value_noinline(receiver_);
		}

		AddressSpaceLockHandle *handle_;
		smarter::shared_ptr<WorkQueue> wq_;
		R receiver_;
	};

	struct [[nodiscard]] AcquireSender {
		using value_type = void;

		template<typename R>
		AcquireOperation<R> connect(R receiver) {
			return {handle, std::move(wq), std::move(receiver)};
		}

		async::sender_awaiter<AcquireSender> operator co_await() {
			return {*this};
		}

		AddressSpaceLockHandle *handle;
		smarter::shared_ptr<WorkQueue> wq;
	};

	AcquireSender acquire(smarter::shared_ptr<WorkQueue> wq) {
		return {this, std::move(wq)};
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
