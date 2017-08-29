
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

QueueSpace::Queue::Queue()
: offset(0) { }

void QueueSpace::submit(frigg::UnsafePtr<AddressSpace> space, Address address,
		QueueNode *node) {
	assert(node->_length % 8 == 0);

	// TODO: do not globally lock the space mutex.
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);
	
	auto pin = ForeignSpaceAccessor{space.toShared(), address, sizeof(QueueStruct)};
	auto qs = DirectSpaceAccessor<unsigned int>{pin, offsetof(QueueStruct, queueLength)};
	auto ks = DirectSpaceAccessor<unsigned int>{pin, offsetof(QueueStruct, kernelState)};
	auto us = DirectSpaceAccessor<unsigned int>{pin, offsetof(QueueStruct, userState)};
	auto next = DirectSpaceAccessor<HelQueue *>{pin, offsetof(QueueStruct, nextQueue)};

	auto ke = __atomic_load_n(ks.get(), __ATOMIC_ACQUIRE);

	// First we traverse the nextQueue list until we find a queue that
	// is has enough free space for our element.
	while((ke & kQueueWantNext)
			|| (ke & kQueueTail) + sizeof(ElementStruct) + node->_length > *qs.get()) {
		if(ke & kQueueWantNext) {
			// Here we wait on the userState futex until the kQueueHasNext bit is set.
			auto ue = __atomic_load_n(us.get(), __ATOMIC_ACQUIRE);
			if(!(ue & kQueueHasNext)) {
				lock.unlock();
				irq_lock.unlock();

				struct Closure : FutexNode {
					Closure(QueueSpace *manager, frigg::UnsafePtr<AddressSpace> space,
							Address address, QueueNode *node)
					: manager{manager}, space{space}, address{address}, node{node} { }

					void onWake() override {
						manager->submit(space, address, node);
					}

					QueueSpace *manager;
					frigg::UnsafePtr<AddressSpace> space;
					Address address;
					QueueNode *node;
				};

				auto futex_node = frigg::construct<Closure>(*kernelAlloc,
						this, space, address, node);
				// FIXME: this can potentially recurse and overflow the stack!
				space->futexSpace.submitWait(address + offsetof(QueueStruct, userState), [&] {
					auto v = __atomic_load_n(us.get(), __ATOMIC_RELAXED);
					return ue == v;
				}, futex_node);
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
					space->futexSpace.wake(address + offsetof(QueueStruct, kernelState));
			}
		}
	}

	unsigned int offset = (ke & kQueueTail);

	auto ez = ForeignSpaceAccessor::acquire(space.toShared(),
			reinterpret_cast<void *>(address + sizeof(QueueStruct) + offset),
			sizeof(ElementStruct));
	ez.copyTo(offsetof(ElementStruct, length), &node->_length, 4);
	ez.copyTo(offsetof(ElementStruct, context), &node->_context, sizeof(uintptr_t));

	auto element_ptr = reinterpret_cast<void *>(address
			+ sizeof(QueueStruct) + offset + sizeof(ElementStruct));
	node->emit(ForeignSpaceAccessor::acquire(space.toShared(),
			element_ptr, node->_length));

	while(true) {
		assert(!(ke & kQueueWantNext));
		assert((ke & kQueueTail) == offset);

		// Try to update the kernel state word here.
		// This CAS potentially resets the waiters bit.
		unsigned int d = offset + node->_length + sizeof(ElementStruct);
		if(__atomic_compare_exchange_n(ks.get(), &ke, d, false,
				__ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
			if(ke & kQueueWaiters)
				space->futexSpace.wake(address + offsetof(QueueStruct, kernelState));
			break;
		}
	}
}

} // namespace thor

