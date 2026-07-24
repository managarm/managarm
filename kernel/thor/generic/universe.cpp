#include <thor-internal/address-space.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/rcu.hpp>
#include <thor-internal/stream.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/universe.hpp>

namespace thor {

namespace {
	constexpr bool logCleanup = false;
}

template<>
AnyDescriptor AnyDescriptor::make<DescriptorType::thread>(
		smarter::shared_ptr<Thread, ActiveHandle> ptr) {
	static_assert(IsRcuProtected<Thread>);
	assert(ptr);

	AnyDescriptor descriptor;
	descriptor.type_ = DescriptorType::thread;
	descriptor.object_ = ptr.get();
	descriptor.ctr_ = &ptr->_activeCtr;
	ptr.release();
	return descriptor;
}

template<>
AnyDescriptor AnyDescriptor::make<DescriptorType::addressSpace>(
		smarter::shared_ptr<AddressSpace, BindableHandle> ptr) {
	static_assert(IsRcuProtected<AddressSpace>);
	assert(ptr);

	AnyDescriptor descriptor;
	descriptor.type_ = DescriptorType::addressSpace;
	descriptor.object_ = ptr.get();
	descriptor.ctr_ = &ptr->_bindableCtr;
	ptr.release();
	return descriptor;
}

template<>
AnyDescriptor AnyDescriptor::make<DescriptorType::lane>(
		smarter::shared_ptr<Stream, LanePolicy> ptr) {
	static_assert(IsRcuProtected<Stream>);
	assert(ptr);

	AnyDescriptor descriptor;
	descriptor.type_ = DescriptorType::lane;
	auto stream = ptr.get();
	descriptor.extra_ = static_cast<uint8_t>(laneOf(ptr));
	descriptor.object_ = stream;
	descriptor.ctr_ = &stream->peerCounter(static_cast<int>(descriptor.extra_));
	ptr.release();
	return descriptor;
}

template<>
std::expected<smarter::shared_ptr<Stream, LanePolicy>, Error>
AnyDescriptor::resolveObject<DescriptorType::lane>() const {
	if(type_ != DescriptorType::lane)
		return std::unexpected{Error::badDescriptor};

	auto stream = static_cast<Stream *>(object_);
	auto lane = static_cast<int>(extra_);
	stream->peerCounter(lane).increment();
	return adoptLane(stream->selfPtr, lane);
}

void AnyDescriptor::releaseOnZero_() {
	switch(type_) {
	case DescriptorType::thread: {
		auto thread = static_cast<Thread *>(object_);
		thread->dispose();
		thread->self.policy().decrement();
		break;
	}
	case DescriptorType::addressSpace: {
		auto space = static_cast<AddressSpace *>(object_);
		space->dispose();
		space->selfPtr.policy().decrement();
		break;
	}
	case DescriptorType::lane: {
		auto stream = static_cast<Stream *>(object_);
		Stream::onPeersZero(stream, static_cast<int>(extra_));
		stream->selfPtr.policy().decrement();
		break;
	}
	default: {
		// All remaining descriptor types hold a reference through the meta object's counter.
		smarter::meta_object_base::from_ctr(ctr_)->finalize();
		break;
	}
	}
}

std::expected<smarter::shared_ptr<Universe>, Error> Universe::create() {
	auto ptr = allocate_rcu_shared<Universe>(*kernelAlloc, CtorToken{});
	ptr->selfPtr_ = ptr;
	return ptr;
}

Universe::Universe(CtorToken) {
	growRoot_(16);
}

auto Universe::slotAt_(uint32_t index) -> Slot * {
	auto root = root_.load(std::memory_order_relaxed);
	auto chunk = root->chunks()[index >> chunkShift].load(std::memory_order_relaxed);
	return &chunk[index & (chunkSize - 1)];
}

void Universe::setLink_(Slot *slot, uint32_t next) {
	auto value = slot->state.load(std::memory_order_relaxed);
	slot->state.store((value & ~slotIndexMask) | next, std::memory_order_relaxed);
}

Handle Universe::attachDescriptor(AnyDescriptor descriptor) {
	assert(descriptor.type() != DescriptorType::none);

	auto irqLock = frg::guard(&irqMutex());
	auto guard = frg::guard(&lock_);

	Slot *slot;
	uint32_t index;
	if(freeHead_ != nilIndex) {
		index = freeHead_;
		slot = slotAt_(index);
		if(freeHead_ == freeTail_) {
			freeHead_ = nilIndex;
			freeTail_ = nilIndex;
		}else{
			freeHead_ = static_cast<uint32_t>(
					slot->state.load(std::memory_order_relaxed) & slotIndexMask);
		}
	}else{
		slot = allocateSlot_();
		// TODO: Fail with an error instead once callers can handle attach failure.
		if(!slot)
			panicLogger() << "thor: Universe ran out of descriptor slots" << frg::endlog;
		index = static_cast<uint32_t>(numSlots_ - 1);
	}

	auto generation = (slot->state.load(std::memory_order_relaxed) & ~invalidMarker)
			>> slotIndexBits;
	auto handle = (generation << slotIndexBits) | index;
	slot->object = descriptor.raw_object();
	slot->ctr = descriptor.raw_ctr();
	slot->type = descriptor.type();
	slot->extra = descriptor.raw_extra();
	// The slot takes ownership of the descriptor's reference.
	descriptor.release();
	slot->state.store(handle, std::memory_order_release);
	return static_cast<Handle>(handle);
}

frg::optional<AnyDescriptor> Universe::detachDescriptor(Handle handle) {
	if(handle <= 0)
		return frg::null_opt;
	auto h = static_cast<uint64_t>(handle);

	auto irqLock = frg::guard(&irqMutex());
	auto guard = frg::guard(&lock_);

	auto slot = slotFor_(h);
	if(!slot || slot->state.load(std::memory_order_relaxed) != h)
		return frg::null_opt;

	AnyDescriptor descriptor{smarter::adopt_rc, slot->type, slot->extra, slot->object, slot->ctr};

	auto generation = h >> slotIndexBits;
	auto index = static_cast<uint32_t>(h & slotIndexMask);
	if(generation + 1 == (uint64_t{1} << generationBits)) {
		// The slot's possible generations are exhausted. It is never reused.
		slot->state.store(invalidMarker | h, std::memory_order_relaxed);
	}else{
		slot->state.store(invalidMarker | ((generation + 1) << slotIndexBits),
				std::memory_order_relaxed);
		if(pendingTail_ != nilIndex) {
			setLink_(slotAt_(pendingTail_), index);
		} else {
			pendingHead_ = index;
		}
		pendingTail_ = index;
		if(!rcuInFlight_) {
			retiringHead_ = pendingHead_;
			retiringTail_ = pendingTail_;
			pendingHead_ = nilIndex;
			pendingTail_ = nilIndex;
			rcuInFlight_ = true;
			// The reference is dropped by recycleRcu_() once the last batch drains.
			selfPtr_.policy().increment();
			submitRcu(this, &recycleRcu_);
		}
	}

	// The caller releases the returned descriptor without holding the lock.
	return descriptor;
}

Universe::~Universe() {
	if(logCleanup)
		debugLogger() << "thor: Universe is deallocated" << frg::endlog;

	auto root = root_.load(std::memory_order_relaxed);
	for(size_t i = 0; i < numSlots_; ++i) {
		auto chunk = root->chunks()[i >> chunkShift].load(std::memory_order_relaxed);
		auto slot = &chunk[i & (chunkSize - 1)];
		auto h = slot->state.load(std::memory_order_relaxed);
		if(h & invalidMarker)
			continue;
		// Dropping the materialized descriptor releases the slot's reference.
		AnyDescriptor descriptor{smarter::adopt_rc, slot->type, slot->extra, slot->object, slot->ctr};
	}

	for(size_t i = 0; i < (numSlots_ + chunkSize - 1) >> chunkShift; ++i) {
		auto chunk = root->chunks()[i].load(std::memory_order_relaxed);
		kernelAlloc->deallocate(chunk, chunkSize * sizeof(Slot));
	}
	kernelAlloc->deallocate(root,
			sizeof(Root) + root->numChunks * sizeof(std::atomic<Slot *>));
}

auto Universe::allocateSlot_() -> Slot * {
	if(numSlots_ == (size_t{1} << slotIndexBits))
		return nullptr;
	auto root = root_.load(std::memory_order_relaxed);
	auto chunkIndex = numSlots_ >> chunkShift;
	if(chunkIndex >= root->numChunks) {
		growRoot_(2 * root->numChunks);
		root = root_.load(std::memory_order_relaxed);
	}
	auto chunk = root->chunks()[chunkIndex].load(std::memory_order_relaxed);
	if(!chunk) {
		chunk = static_cast<Slot *>(kernelAlloc->allocate(chunkSize * sizeof(Slot)));
		for(size_t i = 0; i < chunkSize; ++i)
			new (&chunk[i]) Slot{};
		root->chunks()[chunkIndex].store(chunk, std::memory_order_release);
	}
	auto slot = &chunk[numSlots_ & (chunkSize - 1)];
	++numSlots_;
	return slot;
}

void Universe::growRoot_(size_t numChunks) {
	auto memory = kernelAlloc->allocate(sizeof(Root) + numChunks * sizeof(std::atomic<Slot *>));
	auto root = new (memory) Root{.numChunks = numChunks};

	// Copy over old chunk pointers and initialize new ones to nullptr.
	auto oldRoot = root_.load(std::memory_order_relaxed);
	size_t i = 0;
	if(oldRoot) {
		for(; i < oldRoot->numChunks; ++i) {
			new (&root->chunks()[i]) std::atomic<Slot *>{
					oldRoot->chunks()[i].load(std::memory_order_relaxed)
			};
		}
	}
	for(; i < numChunks; ++i)
		new (&root->chunks()[i]) std::atomic<Slot *>{nullptr};

	// Publish the new root and free the old one using RCU (but not the chunks attached to it).
	root_.store(root, std::memory_order_release);
	if(oldRoot) {
		submitRcu(oldRoot, [] (RcuCallable *base) {
			auto self = static_cast<Root *>(base);
			kernelAlloc->deallocate(self,
					sizeof(Root) + self->numChunks * sizeof(std::atomic<Slot *>));
		});
	}
}

// Runs after a grace period has passed since the retiring batch was closed.
void Universe::recycleRcu_(RcuCallable *base) {
	auto self = static_cast<Universe *>(base);

	bool resubmit;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto guard = frg::guard(&self->lock_);

		if(self->freeTail_ != nilIndex) {
			self->setLink_(self->slotAt_(self->freeTail_), self->retiringHead_);
		} else {
			self->freeHead_ = self->retiringHead_;
		}
		self->freeTail_ = self->retiringTail_;
		self->retiringHead_ = nilIndex;
		self->retiringTail_ = nilIndex;

		resubmit = self->pendingHead_ != nilIndex;
		if(resubmit) {
			self->retiringHead_ = self->pendingHead_;
			self->retiringTail_ = self->pendingTail_;
			self->pendingHead_ = nilIndex;
			self->pendingTail_ = nilIndex;
		} else {
			self->rcuInFlight_ = false;
		}
	}
	if(resubmit) {
		submitRcu(self, &recycleRcu_);
	} else {
		// May destroy the Universe; must not hold lock_ here.
		self->selfPtr_.policy().decrement();
	}
}

} // namespace thor
