
#include "kernel.hpp"

namespace thor {

namespace {

// NOTE: The following structs mirror the Hel{Queue,Element} structs.
// They must be kept in sync!

const unsigned int kQueueWaiters = (unsigned int)1 << 31;
const unsigned int kQueueWantNext = (unsigned int)1 << 30;
const unsigned int kQueueTail = ((unsigned int)1 << 30) - 1;

const unsigned int kQueueHasNext = (unsigned int)1 << 31;

struct QueueStruct {
	unsigned int elementLimit;
	unsigned int queueLength;
	unsigned int kernelState;
	unsigned int userState;
	struct HelQueue *nextQueue;
	char queueBuffer[];
};

struct ElementStruct {
	unsigned int length;
	unsigned int reserved;
	void *context;
};

} // anonymous namespace

QueueSpace::BaseElement::BaseElement(size_t length, uintptr_t context)
: _length(length), _context(context) { }

size_t QueueSpace::BaseElement::getLength() {
	return _length;
}

uintptr_t QueueSpace::BaseElement::getContext() {
	return _context;
}

QueueSpace::Queue::Queue()
: offset(0) { }

void QueueSpace::_submitElement(frigg::UnsafePtr<AddressSpace> space, Address address,
		frigg::SharedPtr<BaseElement> element) {
	auto wake = [&] (uintptr_t p) {
		// FIXME: the mapping needs to be protected after the lock on the AddressSpace is released.
		Mapping *mapping;
		{
			AddressSpace::Guard space_guard(&space->lock);
			mapping = space->getMapping(p);
		}
		assert(mapping->type == Mapping::kTypeMemory);

		auto futex = &mapping->memoryRegion->futex;
		futex->wake(p - mapping->baseAddress);
	};
	
	auto waitIf = [&] (uintptr_t p, auto c, auto f) {
		// FIXME: the mapping needs to be protected after the lock on the AddressSpace is released.
		Mapping *mapping;
		{
			AddressSpace::Guard space_guard(&space->lock);
			mapping = space->getMapping(p);
		}
		assert(mapping->type == Mapping::kTypeMemory);

		auto futex = &mapping->memoryRegion->futex;
		futex->waitIf(p - mapping->baseAddress, frigg::move(c), frigg::move(f));
	};

	// TODO: do not globally lock the space mutex.
	auto lock = frigg::guard(&_mutex);
	
	unsigned int length = element->getLength();
	uintptr_t context = element->getContext();
	assert(length % 8 == 0);
	
	auto pin = ForeignSpaceAccessor{space.toShared(), address, sizeof(QueueStruct)};
	auto qs = DirectSpaceAccessor<unsigned int>{pin, offsetof(QueueStruct, queueLength)};
	auto ks = DirectSpaceAccessor<unsigned int>{pin, offsetof(QueueStruct, kernelState)};
	auto us = DirectSpaceAccessor<unsigned int>{pin, offsetof(QueueStruct, userState)};
	auto next = DirectSpaceAccessor<HelQueue *>{pin, offsetof(QueueStruct, nextQueue)};

	auto ke = __atomic_load_n(ks.get(), __ATOMIC_ACQUIRE);

	// First we traverse the nextQueue list until we find a queue that
	// is has enough free space for our element.
	while((ke & kQueueWantNext)
			|| (ke & kQueueTail) + sizeof(ElementStruct) + length > *qs.get()) {
		if(ke & kQueueWantNext) {
			// Here we wait on the userState futex until the kQueueHasNext bit is set.
			auto ue = __atomic_load_n(us.get(), __ATOMIC_ACQUIRE);
			if(!(ue & kQueueHasNext)) {
				waitIf(address + offsetof(QueueStruct, userState), [&] {
					auto v = __atomic_load_n(us.get(), __ATOMIC_RELAXED);
					return ue == v;
				}, [this, space, address, element = frigg::move(element)] {
					// FIXME: this can potentially recurse and overflow the stack!
					_submitElement(space, address, frigg::move(element));
				});
				return;
			}

			// Finally we move to the next queue.
			address = Address(*next.get());
	
			pin = ForeignSpaceAccessor{space.toShared(), address, sizeof(QueueStruct)};
			qs = DirectSpaceAccessor<unsigned int>{pin, offsetof(QueueStruct, queueLength)};
			ks = DirectSpaceAccessor<unsigned int>{pin, offsetof(QueueStruct, kernelState)};
			us = DirectSpaceAccessor<unsigned int>{pin, offsetof(QueueStruct, userState)};
			next = DirectSpaceAccessor<HelQueue *>{pin, offsetof(QueueStruct, nextQueue)};
			
			ke = __atomic_load_n(ks.get(), __ATOMIC_ACQUIRE);
		}else{
			// Set the kQueueWantNext bit. If this happens we will usually
			// wait on the userState futex in the next while-iteration.
			unsigned int d = ke | kQueueWantNext;
			if(__atomic_compare_exchange_n(ks.get(), &ke, d, false,
					__ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
				if(ke & kQueueWaiters)
					wake(address + offsetof(QueueStruct, kernelState));
			}
		}
	}

	unsigned int offset = (ke & kQueueTail);

	auto ez = ForeignSpaceAccessor::acquire(space.toShared(),
			reinterpret_cast<void *>(address + sizeof(QueueStruct) + offset),
			sizeof(ElementStruct));
	ez.copyTo(offsetof(ElementStruct, length), &length, 4);
	ez.copyTo(offsetof(ElementStruct, context), &context, sizeof(uintptr_t));

	auto element_ptr = reinterpret_cast<void *>(address
			+ sizeof(QueueStruct) + offset + sizeof(ElementStruct));
	element->complete(ForeignSpaceAccessor::acquire(space.toShared(),
			element_ptr, length));

	while(true) {
		assert(!(ke & kQueueWantNext));
		assert((ke & kQueueTail) == offset);

		// Try to update the kernel state word here.
		// This CAS potentially resets the waiters bit.
		unsigned int d = offset + length + sizeof(ElementStruct);
		if(__atomic_compare_exchange_n(ks.get(), &ke, d, false,
				__ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
			if(ke & kQueueWaiters)
				wake(address + offsetof(QueueStruct, kernelState));
			break;
		}
	}
}

} // namespace thor

