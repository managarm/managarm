
#include "kernel.hpp"

namespace thor {

QueueSpace::BaseElement::BaseElement(size_t length)
: _length(length) { }

size_t QueueSpace::BaseElement::getLength() {
	return _length;
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
	frigg::infoLogger() << "HelQueue: Writing to " << offset << frigg::endLog;
	
	auto ez = ForeignSpaceAccessor::acquire(space.toShared(),
			reinterpret_cast<void *>(address + 24 + offset + 0), 4);
	unsigned int length = element->getLength();
	ez.copyTo(0, &length, 4);

	auto accessor = ForeignSpaceAccessor::acquire(space.toShared(),
			reinterpret_cast<void *>(address + 24 + offset + 24), element->getLength());
	element->complete(frigg::move(accessor));

	unsigned int expected = offset;
	while(true) {
		if(__atomic_compare_exchange_n(ks.get(), &expected, it->offset, false,
				__ATOMIC_RELEASE, __ATOMIC_RELAXED))
			break;
		assert(!"HelQueue: Kernel CAS failed");
	}
}

} // namespace thor

