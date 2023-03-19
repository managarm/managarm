#include <arch/dma_pool.hpp>
#include <async/basic.hpp>
#include <assert.h>
#include <stdint.h>
#include <nic/i8254x/common.hpp>
#include <nic/i8254x/regs.hpp>
#include <nic/i8254x/tx.hpp>

namespace {

async::detached raiseEvent(Request *request) {
	if constexpr (logDebug) std::cout << "i8254x: TX RAISE EVENT" << std::endl;
	request->event.raise();
	co_return;
}

}

TxQueue::TxQueue(size_t descriptors, Intel8254xNic &nic) : _nic{nic}, _requests{}, _descriptor_count(descriptors) {
	auto pool = _nic.dmaPool();
	_descriptors = arch::dma_array<TxDescriptor>(pool, _descriptor_count);
	_descriptor_buffers = arch::dma_array<DescriptorSpace>(pool, _descriptor_count);

	for(size_t i = 0; i < _descriptor_count; i++) {
		_descriptors[i].address = helix_ng::ptrToPhysical(&_descriptor_buffers[i]);
	}
};

async::result<void> TxQueue::submitDescriptor(arch::dma_buffer_view payload, Intel8254xNic &nic) {
	Request ev_req(_descriptor_count);

	co_await postDescriptor(payload, nic, &ev_req, raiseEvent);
	co_await ev_req.event.wait();
}

async::result<void> TxQueue::postDescriptor(arch::dma_buffer_view payload, Intel8254xNic &nic, Request *req, async::detached (*complete)(Request *)) {
	auto head = this->head();
	auto tail = this->tail();
	auto next = tail + 1;

	if constexpr (logDebug) printf("i8254x/TxQueue: tx head=%zu tail=%zu next=%zu\n", head(), tail(), next());

	req->complete = complete;
	req->index = QueueIndex(tail, _descriptor_count);

	_requests.push(req);

	auto desc = &_descriptors[tail];

	memcpy(getDescriptorPtr(tail()), payload.data(), payload.size());

	desc->status = flags::tx::status::done(false);
	desc->length = payload.size();
	desc->cmd = flags::tx::cmd::report_status(true) | flags::tx::cmd::insert_fcs(true) | flags::tx::cmd::end_of_packet(true);

	asm volatile ( "" : : : "memory" );

	nic._mmio.store(regs::tdt, next);

	co_return;
}

void TxQueue::ackAll() {
	while(!_requests.empty()) {
		auto request = _requests.front();
		assert(request);

		if constexpr (logDebug) std::cout << "i8254x/TxQueue: checking tx desc id " << request->index() << std::endl;

		auto desc = &_descriptors[request->index()];
		assert(desc);

		if(desc->status & flags::tx::status::done) {
			if constexpr (logDebug) std::cout << "i8254x/TxQueue: ACKing tx desc id " << request->index() << std::endl;
			request->complete(request);
			_requests.pop();
		} else {
			/* if we reach TX descriptors that are still in-flight, we return in order to wait for the next interrupt to occur */
			if constexpr (logDebug) std::cout << "i8254x/TxQueue: descriptor not ready id " << request->index() << std::endl;
			return;
		}
	}
}

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
