
#include "kernel.hpp"

namespace thor {

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
	// TODO: do not globally lock the space mutex.
	auto lock = frigg::guard(&_mutex);
	
	// TODO: do not use magic number for field offsets here.
	auto qs = DirectSpaceAccessor<unsigned int>::acquire(space.toShared(),
			reinterpret_cast<unsigned int *>(address + 4));
	auto ks = DirectSpaceAccessor<unsigned int>::acquire(space.toShared(),
			reinterpret_cast<unsigned int *>(address + 8));
	
	// TODO: handle linked queues.
	auto it = _queues.get(address);
	if(!it) {
		_queues.insert(address, Queue());
		it = _queues.get(address);
	}
	
	size_t offset = it->offset;
	assert(offset % 8 == 0);
	assert(offset + element->getLength() + 24 <= *qs.get());
	it->offset += element->getLength() + 24;
	
	auto ez = ForeignSpaceAccessor::acquire(space.toShared(),
			reinterpret_cast<void *>(address + 24 + offset), 16);
	unsigned int length = element->getLength();
	uintptr_t context = element->getContext();
	ez.copyTo(0, &length, 4);
	ez.copyTo(8, &context, sizeof(uintptr_t));

	auto accessor = ForeignSpaceAccessor::acquire(space.toShared(),
			reinterpret_cast<void *>(address + 24 + offset + 24), element->getLength());
	element->complete(frigg::move(accessor));

	const unsigned int kQueueWaiters = (unsigned int)1 << 31;
	const unsigned int kQueueTail = ((unsigned int)1 << 30) - 1;

	unsigned int e = offset;
	while(true) {
		assert((e & kQueueTail) == offset);

		// try to update the kernel state word here.
		// this CAS potentially resets the waiters bit.
		unsigned int d = it->offset;
		if(__atomic_compare_exchange_n(ks.get(), &e, d, false,
				__ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
			if(e & kQueueWaiters) {
				AddressSpace::Guard space_guard(&space->lock);
				auto mapping = space->getMapping(address + 8);
				assert(mapping->type == Mapping::kTypeMemory);

				auto futex = &mapping->memoryRegion->futex;
				futex->wake(address + 8 - mapping->baseAddress);
			}

			break;
		}
	}
}

} // namespace thor

