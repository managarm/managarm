#pragma once

#include <helix/memory.hpp>
#include <nic/rtl8168/common.hpp>
#include <nic/rtl8168/descriptor.hpp>
#include <queue>

struct RealtekNic;

struct TxQueue {
	TxQueue(size_t descriptors, RealtekNic &nic);

	void handleTxOk();

	uintptr_t getBase() {
		return helix_ng::ptrToPhysical(&_descriptors[0]);
	}

	async::result<void> submitDescriptor(arch::dma_buffer_view frame, RealtekNic &nic);
	async::result<void> postDescriptor(arch::dma_buffer_view frame, RealtekNic &nic, std::shared_ptr<Request> req);

	bool bufferEmpty() {
		return _amount_free_descriptors == _descriptor_count;
	}
protected:
	size_t _descriptor_count;
	size_t _amount_free_descriptors;
	std::vector<arch::dma_buffer> _descriptor_buffers;
	std::queue<std::shared_ptr<Request>> _requests;
	arch::dma_array<Descriptor> _descriptors;
	QueueIndex tx_index;    // Our index into the TX buffer
	QueueIndex hw_tx_index; // The index into the TX buffer that the card currently has
};
