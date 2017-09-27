
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
	QueueStruct *nextQueue;
	char queueBuffer[];
};

struct ElementStruct {
	unsigned int length;
	unsigned int reserved;
	void *context;
};

} // anonymous namespace

void QueueSpace::Slot::onWake() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&manager->_mutex);

	waitInFutex = false;
	manager->_progress(this);
}

void QueueSpace::submit(frigg::UnsafePtr<AddressSpace> space, Address address,
		QueueNode *node) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	auto it = _slots.get(address);
	if(!it) {
		_slots.insert(address, Slot{this, space, address});
		it = _slots.get(address);
	}

	assert(!node->_queueNode.in_list);
//	frigg::infoLogger() << "[" << this << "] push " << node << frigg::endLog;
	it->queue.push_back(node);

	_progress(it);
}

void QueueSpace::_progress(Slot *slot) {
	while(!slot->queue.empty()) {
		Address successor = 0;
		NodeList migrate_list;
		if(!_progressFront(slot, successor, migrate_list))
			return;

		if(successor) {
			// Allocate a new slot and migrate all existing nodes.
			auto it = _slots.get(successor);
			if(!it) {
				_slots.insert(successor, Slot{this, slot->space, successor});
				it = _slots.get(successor);
			}

			assert(!migrate_list.empty());
			while(!migrate_list.empty()) {
//				frigg::infoLogger() << "[" << this << "] migrate "
//						<< migrate_list.back() << frigg::endLog;
				it->queue.push_front(migrate_list.pop_back());
				assert(it->queue.front()->_queueNode.in_list);
			}

			slot = it;
		}
	}
}

bool QueueSpace::_progressFront(Slot *slot, Address &successor, NodeList &migrate_list) {
	assert(!slot->queue.empty());
	auto address = slot->address;

	size_t length = 0;
	for(auto chunk = slot->queue.front()->_chunk; chunk; chunk = chunk->link)
		length += (chunk->size + 7) & ~size_t(7);
	assert(length % 8 == 0);
	auto context = slot->queue.front()->_context;

	auto pin = ForeignSpaceAccessor{slot->space.toShared(), address, sizeof(QueueStruct)};
	auto qs = DirectSpaceAccessor<unsigned int>{pin, offsetof(QueueStruct, queueLength)};
	auto ks = DirectSpaceAccessor<unsigned int>{pin, offsetof(QueueStruct, kernelState)};
	auto us = DirectSpaceAccessor<unsigned int>{pin, offsetof(QueueStruct, userState)};
	auto next = DirectSpaceAccessor<QueueStruct *>{pin, offsetof(QueueStruct, nextQueue)};

	auto ke = __atomic_load_n(ks.get(), __ATOMIC_ACQUIRE);

	if(slot->waitInFutex)
		return false;

	// First we traverse the nextQueue list until we find a queue that
	// is has enough free space for our element.
	while((ke & kQueueWantNext)
			|| (ke & kQueueTail) + sizeof(ElementStruct) + length > *qs.get()) {
		if(ke & kQueueWantNext) {
			// Here we wait on the userState futex until the kQueueHasNext bit is set.
			auto ue = __atomic_load_n(us.get(), __ATOMIC_ACQUIRE);
			if(!(ue & kQueueHasNext)) {
				// We need checkSubmitWait() to avoid a deadlock that is triggered
				// by taking locks in onWake().
				slot->waitInFutex = slot->space->futexSpace.checkSubmitWait(address
						+ offsetof(QueueStruct, userState), [&] {
					auto v = __atomic_load_n(us.get(), __ATOMIC_RELAXED);
					return ue == v;
				}, slot);
				return !slot->waitInFutex;
			}

			// Finally we move to the next queue.
			successor = Address(*next.get());
			while(!slot->queue.empty()) {
//				frigg::infoLogger() << "[" << this << "] erase "
//						<< slot->queue.front() << frigg::endLog;
				assert(slot->queue.front()->_queueNode.in_list);
				migrate_list.push_back(slot->queue.pop_front());
			}
			return true;
		}else{
			// Set the kQueueWantNext bit. If this happens we will usually
			// wait on the userState futex in the next while-iteration.
			unsigned int d = ke | kQueueWantNext;
			if(__atomic_compare_exchange_n(ks.get(), &ke, d, false,
					__ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
				if(ke & kQueueWaiters)
					slot->space->futexSpace.wake(address + offsetof(QueueStruct, kernelState));
			}
		}
	}

	unsigned int offset = (ke & kQueueTail);

	auto ez = ForeignSpaceAccessor::acquire(slot->space.toShared(),
			reinterpret_cast<void *>(address + sizeof(QueueStruct) + offset),
			sizeof(ElementStruct));

	Error err;
	err = ez.write(offsetof(ElementStruct, length), &length, 4);
	assert(!err);
	err = ez.write(offsetof(ElementStruct, context), &context, sizeof(uintptr_t));
	assert(!err);
	
//	frigg::infoLogger() << "[" << this << "] finish " << slot->queue.front() << frigg::endLog;
	auto node = slot->queue.pop_front();

	auto accessor = ForeignSpaceAccessor::acquire(slot->space.toShared(),
			reinterpret_cast<void *>(address + sizeof(QueueStruct) + offset
					+ sizeof(ElementStruct)),
			length);
	size_t disp = 0;
	for(auto chunk = node->_chunk; chunk; chunk = chunk->link) {
		accessor.write(disp, chunk->pointer, chunk->size);
		disp += (chunk->size + 7) & ~size_t(7);
	}

	node->complete();

	while(true) {
		assert(!(ke & kQueueWantNext));
		assert((ke & kQueueTail) == offset);

		// Try to update the kernel state word here.
		// This CAS potentially resets the waiters bit.
		unsigned int d = offset + length + sizeof(ElementStruct);
		if(__atomic_compare_exchange_n(ks.get(), &ke, d, false,
				__ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
			if(ke & kQueueWaiters)
				slot->space->futexSpace.wake(address + offsetof(QueueStruct, kernelState));
			break;
		}
	}

	return !slot->queue.empty();
}

} // namespace thor

