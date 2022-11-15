#include <helix/memory.hpp>
#include <nic/i8254x/common.hpp>
#include <nic/i8254x/rx.hpp>

RxQueue::RxQueue(size_t descriptors, Intel8254xNic &nic)
: _nic{nic}, _requests{}, _descriptor_count(descriptors), next_index(0, descriptors) {
	auto pool = _nic.dmaPool();
	_descriptors = arch::dma_array<RxDescriptor>(pool, _descriptor_count);
	_descriptor_buffers = arch::dma_array<DescriptorSpace>(pool, _descriptor_count);

	for(size_t i = 0; i < _descriptor_count; i++) {
		_descriptors[i].address = helix_ng::ptrToPhysical(&_descriptor_buffers[i]);
	}
};

uintptr_t RxQueue::getBase() {
	return helix_ng::ptrToPhysical(&_descriptors[0]);
}

QueueIndex RxQueue::head() {
	return {_nic._mmio.load(regs::rdh), _descriptor_count };
}

QueueIndex RxQueue::tail() {
	return {_nic._mmio.load(regs::rdt), _descriptor_count };
}

void RxQueue::tail(QueueIndex i) {
	_nic._mmio.store(regs::rdt, i());
}
