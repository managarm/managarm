#include <arch/dma_pool.hpp>
#include <async/basic.hpp>
#include <assert.h>
#include <stdint.h>
#include <nic/i8254x/common.hpp>
#include <nic/i8254x/tx.hpp>

TxQueue::TxQueue(size_t descriptors, Intel8254xNic &nic) : _nic{nic}, _requests{}, _descriptor_count(descriptors) {
	auto pool = _nic.dmaPool();
	_descriptors = arch::dma_array<TxDescriptor>(pool, _descriptor_count);
	_descriptor_buffers = arch::dma_array<DescriptorSpace>(pool, _descriptor_count);

	for(size_t i = 0; i < _descriptor_count; i++) {
		_descriptors[i].address = helix_ng::ptrToPhysical(&_descriptor_buffers[i]);
	}
};

uintptr_t TxQueue::getBase() {
	return helix_ng::ptrToPhysical(&_descriptors[0]);
}

void *TxQueue::getDescriptorPtr(size_t index) {
	return reinterpret_cast<void *>(&_descriptor_buffers[index]);
}

QueueIndex TxQueue::head() {
	return {_nic._mmio.load(regs::tdh), _descriptor_count};
}

QueueIndex TxQueue::tail() {
	return {_nic._mmio.load(regs::tdt), _descriptor_count};
}
