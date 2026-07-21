#pragma once

#include <expected>
#include <optional>
#include <stdint.h>
#include <type_traits>
#include <utility>
#include <frg/optional.hpp>
#include <assert.h>
#include <smarter.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/mm-rc.hpp>
#include <thor-internal/virtualization.hpp>

namespace thor {

typedef int64_t Handle;

struct MemoryView;
struct AddressSpace;
struct IoSpace;
struct Thread;
struct Universe;
struct IpcQueue;
struct MemorySlice;
struct NamedMemoryViewLock;
struct KernletObject;
struct BoundKernlet;
struct TokenObject;
struct DmaSpace;
struct IrqObject;
struct OneshotEvent;
struct BitsetEvent;

// --------------------------------------------------------
// Lane handles.
// --------------------------------------------------------

struct StreamControl;
struct Stream;

// Refcount policy for smarter::shared_ptr.
// A lane handle is a Stream pointer plus a lane index.
// The refcount that it manipulates is the lane's peer counter.
struct LanePolicy {
	LanePolicy() = default;

	LanePolicy(Stream *stream, int lane)
	: stream_{stream}, lane_{lane} { }

	explicit operator bool () const {
		return stream_;
	}

	void increment() const;
	void decrement() const;

	Stream *stream() const {
		return stream_;
	}

	int lane() const {
		return lane_;
	}

private:
	Stream *stream_ = nullptr;
	int lane_ = -1;
};
static_assert(smarter::rc_policy<LanePolicy>);

// Constructs a lane handle that adopts an existing peer reference on the stream.
inline smarter::shared_ptr<Stream, LanePolicy> adoptLane(
		smarter::borrowed_ptr<Stream> stream, int lane) {
	return smarter::shared_ptr<Stream, LanePolicy>{
			smarter::adopt_rc, stream.get(), LanePolicy{stream.get(), lane}};
}

// Extracts the numeric lane index of a lane handle.
inline int laneOf(const smarter::shared_ptr<Stream, LanePolicy> &lane) {
	return lane.policy().lane();
}

// --------------------------------------------------------
// AnyDescriptor
// --------------------------------------------------------

enum class DescriptorType : uint8_t {
	none,
	universe,
	queue,
	memoryView,
	memorySlice,
	addressSpace,
	virtualizedSpace,
	dmaSpace,
	virtualizedCpu,
	memoryViewLock,
	thread,
	lane,
	irq,
	oneshotEvent,
	bitsetEvent,
	io,
	kernletObject,
	boundKernlet,
	token,
};

// Maps a descriptor type to the object type and the smarter::shared_ptr refcount
// policy that a descriptor of that type holds.
template<DescriptorType K>
struct DescriptorTraits;

template<>
struct DescriptorTraits<DescriptorType::universe> {
	using Object = Universe;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::queue> {
	using Object = IpcQueue;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::memoryView> {
	using Object = MemoryView;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::memorySlice> {
	using Object = MemorySlice;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::addressSpace> {
	using Object = AddressSpace;
	using Policy = BindableHandle;
};

template<>
struct DescriptorTraits<DescriptorType::virtualizedSpace> {
	using Object = VirtualizedPageSpace;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::dmaSpace> {
	using Object = DmaSpace;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::virtualizedCpu> {
	using Object = VirtualizedCpu;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::memoryViewLock> {
	using Object = NamedMemoryViewLock;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::thread> {
	using Object = Thread;
	using Policy = ActiveHandle;
};

template<>
struct DescriptorTraits<DescriptorType::lane> {
	using Object = Stream;
	using Policy = LanePolicy;
};

template<>
struct DescriptorTraits<DescriptorType::irq> {
	using Object = IrqObject;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::oneshotEvent> {
	using Object = OneshotEvent;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::bitsetEvent> {
	using Object = BitsetEvent;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::io> {
	using Object = IoSpace;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::kernletObject> {
	using Object = KernletObject;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::boundKernlet> {
	using Object = BoundKernlet;
	using Policy = smarter::default_rc_policy;
};

template<>
struct DescriptorTraits<DescriptorType::token> {
	using Object = TokenObject;
	using Policy = smarter::default_rc_policy;
};

// smarter::shared_ptr type that a descriptor of type K holds.
template<DescriptorType K>
using DescriptorPointer = smarter::shared_ptr<
	typename DescriptorTraits<K>::Object,
	typename DescriptorTraits<K>::Policy
>;

struct AnyDescriptor {
	friend void swap(AnyDescriptor &x, AnyDescriptor &y) {
		using std::swap;
		swap(x.type_, y.type_);
		swap(x.extra_, y.extra_);
		swap(x.object_, y.object_);
		swap(x.ctr_, y.ctr_);
	}

	// Constructs a descriptor of type K that takes over the given pointer's reference.
	template<DescriptorType K>
	static AnyDescriptor make(DescriptorPointer<K> ptr);

	AnyDescriptor() = default;

	AnyDescriptor(const AnyDescriptor &other)
	: type_{other.type_}, extra_{other.extra_}, object_{other.object_}, ctr_{other.ctr_} {
		if(ctr_)
			ctr_->increment();
	}

	AnyDescriptor(AnyDescriptor &&other)
	: AnyDescriptor{} {
		swap(*this, other);
	}

	~AnyDescriptor() {
		if(ctr_ && ctr_->decrement_and_check_if_zero())
			releaseOnZero_();
	}

	AnyDescriptor &operator= (AnyDescriptor other) {
		swap(*this, other);
		return *this;
	}

	DescriptorType type() const {
		return type_;
	}

	template<DescriptorType K>
	bool is() const {
		return type_ == K;
	}

	// Resolves the descriptor to the object it holds (takes a new reference).
	// Fails with badDescriptor unless the descriptor is of type K.
	template<DescriptorType K>
	std::expected<DescriptorPointer<K>, Error> resolveObject() const;

private:
	void releaseOnZero_();

	DescriptorType type_ = DescriptorType::none;
	// Extra per-descriptor data for some descriptor types.
	// - For lane descriptors: the lane index.
	uint8_t extra_ = 0;
	// Invariant: object_ is non-null if type_ != DescriptorType::none.
	void *object_ = nullptr;
	// Invariant: ctr_ is non-null if type_ != DescriptorType::none.
	smarter::counter *ctr_ = nullptr;
};

template<DescriptorType K>
AnyDescriptor AnyDescriptor::make(DescriptorPointer<K> ptr) {
	static_assert(std::same_as<typename DescriptorTraits<K>::Policy, smarter::default_rc_policy>);
	assert(ptr);

	AnyDescriptor descriptor;
	descriptor.type_ = K;
	descriptor.object_ = ptr.get();
	descriptor.ctr_ = &ptr.policy().base()->ctr();
	ptr.release();
	return descriptor;
}

template<>
AnyDescriptor AnyDescriptor::make<DescriptorType::thread>(
		smarter::shared_ptr<Thread, ActiveHandle> ptr);

template<>
AnyDescriptor AnyDescriptor::make<DescriptorType::addressSpace>(
		smarter::shared_ptr<AddressSpace, BindableHandle> ptr);

template<>
AnyDescriptor AnyDescriptor::make<DescriptorType::lane>(
		smarter::shared_ptr<Stream, LanePolicy> ptr);

template<DescriptorType K>
std::expected<DescriptorPointer<K>, Error> AnyDescriptor::resolveObject() const {
	static_assert(std::same_as<typename DescriptorTraits<K>::Policy, smarter::default_rc_policy>);
	using ObjectType = typename DescriptorTraits<K>::Object;

	if(type_ != K)
		return std::unexpected{Error::badDescriptor};
	ctr_->increment();
	return smarter::shared_ptr<ObjectType>{
		smarter::adopt_rc,
		static_cast<ObjectType *>(object_),
		smarter::default_rc_policy{smarter::meta_object_base::from_ctr(ctr_)}
	};
}

template<>
inline std::expected<smarter::shared_ptr<Thread, ActiveHandle>, Error>
AnyDescriptor::resolveObject<DescriptorType::thread>() const {
	if(type_ != DescriptorType::thread)
		return std::unexpected{Error::badDescriptor};
	auto thread = static_cast<Thread *>(object_);
	ctr_->increment();
	return smarter::shared_ptr<Thread, ActiveHandle>{
		smarter::adopt_rc, thread, ActiveHandle{thread}
	};
}

template<>
inline std::expected<smarter::shared_ptr<AddressSpace, BindableHandle>, Error>
AnyDescriptor::resolveObject<DescriptorType::addressSpace>() const {
	if(type_ != DescriptorType::addressSpace)
		return std::unexpected{Error::badDescriptor};
	auto space = static_cast<AddressSpace *>(object_);
	ctr_->increment();
	return smarter::shared_ptr<AddressSpace, BindableHandle>{
		smarter::adopt_rc, space, BindableHandle{space}
	};
}

template<>
std::expected<smarter::shared_ptr<Stream, LanePolicy>, Error>
AnyDescriptor::resolveObject<DescriptorType::lane>() const;

// --------------------------------------------------------
// Universe.
// --------------------------------------------------------

struct Universe {
private:
	struct CtorToken {};

public:
	typedef frg::ticket_spinlock Lock;
	typedef frg::unique_lock<frg::ticket_spinlock> Guard;

	static std::expected<smarter::shared_ptr<Universe>, Error> create();

	Universe(CtorToken);
	~Universe();

	Handle attachDescriptor(AnyDescriptor descriptor);

	std::optional<AnyDescriptor> getDescriptor(Handle handle);

	template<typename Fn>
	requires requires(Fn fn, AnyDescriptor &desc) {
		{ fn(desc) };
	}
	auto inspectDescriptor(Handle handle, Fn &&fn)
			-> std::invoke_result_t<Fn, AnyDescriptor &> {
		using ResultType = std::invoke_result_t<Fn, AnyDescriptor &>;

		auto irqLock = frg::guard(&irqMutex());
		Guard guard(lock);

		auto *desc = _descriptorMap.get(handle);
		if(!desc)
			return ResultType{std::unexpect, Error::noDescriptor};
		return std::forward<Fn>(fn)(*desc);
	}

	// Looks up a handle and resolves it to the object held by its descriptor.
	// Fails with badDescriptor unless the descriptor is of type K.
	template<DescriptorType K>
	std::expected<DescriptorPointer<K>, Error> resolveObject(Handle handle) {
		return inspectDescriptor(handle, [](AnyDescriptor &desc) {
			return desc.resolveObject<K>();
		});
	}

	frg::optional<AnyDescriptor> detachDescriptor(Handle handle);

	Lock lock;

private:
	frg::hash_map<
		Handle,
		AnyDescriptor,
		frg::hash<Handle>,
		KernelAlloc
	> _descriptorMap;

	Handle _nextHandle;
};

} // namespace thor
