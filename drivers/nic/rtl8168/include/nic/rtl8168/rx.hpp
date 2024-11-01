#pragma once

#include <helix/memory.hpp>
#include <nic/rtl8168/common.hpp>
#include <nic/rtl8168/descriptor.hpp>
#include <queue>

struct RealtekNic;

struct RxQueue {
	RxQueue(size_t descriptors, RealtekNic &nic);

	uintptr_t getBase() { return helix_ng::ptrToPhysical(&_descriptors[0]); }

	void handleRxOk();
	bool checkOwnerOfNextDescriptor();
	async::result<size_t> submitDescriptor(arch::dma_buffer_view frame, RealtekNic &nic);
	async::result<void>
	postDescriptor(arch::dma_buffer_view frame, RealtekNic &nic, std::shared_ptr<Request> req);

  private:
	size_t _descriptor_count;
	std::vector<arch::dma_buffer> _descriptor_buffers;
	std::queue<std::shared_ptr<Request>> _requests;
	arch::dma_array<Descriptor> _descriptors;
	QueueIndex _last_rx_index;
	QueueIndex _next_index;
};
