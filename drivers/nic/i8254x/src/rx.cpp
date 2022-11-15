#include <helix/memory.hpp>
#include <nic/i8254x/common.hpp>
#include <nic/i8254x/rx.hpp>
#include <nic/i8254x/regs.hpp>

namespace {

async::detached raiseEvent(Request *request) {
	if constexpr (logDebug) std::cout << "i8254x: RX RAISE EVENT" << std::endl;
	request->event.raise();
	co_return;
}

}

RxQueue::RxQueue(size_t descriptors, Intel8254xNic &nic)
: _nic{nic}, _requests{}, _descriptor_count(descriptors), next_index(0, descriptors) {
	auto pool = _nic.dmaPool();
	_descriptors = arch::dma_array<RxDescriptor>(pool, _descriptor_count);
	_descriptor_buffers = arch::dma_array<DescriptorSpace>(pool, _descriptor_count);

	for(size_t i = 0; i < _descriptor_count; i++) {
		_descriptors[i].address = helix_ng::ptrToPhysical(&_descriptor_buffers[i]);
	}
};

async::result<void> RxQueue::submitDescriptor(arch::dma_buffer_view frame, Intel8254xNic &nic) {
	Request ev_req(_descriptor_count);

	co_await postDescriptor(frame, nic, &ev_req, raiseEvent);

	co_await ev_req.event.wait();
}

async::result<void> RxQueue::postDescriptor(arch::dma_buffer_view frame, Intel8254xNic &nic, Request *req, async::detached (*complete)(Request *)) {
	req->complete = complete;
	req->frame = frame;
	req->index = next_index;

	auto head = this->head();
	auto tail = this->tail();
	auto next = next_index;

	if constexpr (logDebug) printf("i8254x/RxQueue: rx post head=%zu tail=%zu next=%zu\n", head(), tail(), next());

	_requests.push(req);

	++next_index;

	co_return;
}

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
