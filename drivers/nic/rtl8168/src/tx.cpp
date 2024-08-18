
#include <linux/if_link.h>
#include <memory>
#include <nic/rtl8168/common.hpp>
#include <nic/rtl8168/tx.hpp>
#include <nic/rtl8168/descriptor.hpp>
#include <nic/rtl8168/regs.hpp>
#include <nic/rtl8168/debug_options.hpp>

void RealtekNic::setTxConfigRegisters() {
	auto val = flags::transmit_config::mxdma(flags::transmit_config::mxdma_burst) |
		flags::transmit_config::ifg(flags::transmit_config::ifg_normal);

	if (_revision >= MacRevision::MacVer34 &&
		_revision != MacRevision::MacVer39 &&
		_revision <= MacRevision::MacVer53) {
		val |= flags::transmit_config::auto_fifo(true);
	}

	_mmio.store(regs::transmit_config, val);
}

TxQueue::TxQueue(size_t descriptors, RealtekNic &nic) : _descriptor_count{descriptors}, _amount_free_descriptors{descriptors}, tx_index{0, descriptors}, hw_tx_index{0, descriptors} {
	_descriptors = arch::dma_array<Descriptor>(nic.dmaPool(), _descriptor_count);
	_descriptor_buffers.reserve(_descriptor_count);

	for(size_t i = 0; i < _descriptor_count; i++) {
		auto buf = arch::dma_buffer(nic.dmaPool(), 2048);
		memset(buf.data(), 0, buf.size());
		uintptr_t addr = helix_ng::ptrToPhysical(buf.data());

		_descriptor_buffers.push_back(std::move(buf));

		_descriptors[i].flags = flags::tx::frame_length(0);
		_descriptors[i].vlan = 0;
		_descriptors[i].base_low = addr & 0xFFFF'FFFF;
		_descriptors[i].base_high = (addr >> 32) & 0xFFFF'FFFF;
	}

	_descriptors[_descriptor_count - 1].flags |= flags::tx::eor(true);
}

async::result<void> TxQueue::submitDescriptor(arch::dma_buffer_view payload, RealtekNic &nic) {
	auto ev_req = std::make_shared<Request>(_descriptor_count);

	co_await postDescriptor(payload, nic, ev_req);
	co_await ev_req->event.wait();
}

// TODO: support large packets
// TODO: this function should be able to fail; there may not be enough space in the ring buffer, which should be handled gracefully
async::result<void> TxQueue::postDescriptor(arch::dma_buffer_view payload, RealtekNic &nic, std::shared_ptr<Request> req) {
	assert(_amount_free_descriptors);

	_requests.push(req);

	auto desc = &_descriptors[tx_index()];

	// Manually pad the buffer
	// This bypasses issues on some cards.
	// TODO: gate this to only the cards which have these issues, and only do it if these issues would occur
	auto actual_size = payload.size() > 60 ? payload.size() : 60;

	if(actual_size != payload.size()) {
		memset(_descriptor_buffers[tx_index()].data(), 0, actual_size);
	}

	memcpy(_descriptor_buffers[tx_index()].data(), payload.data(), payload.size());

	// Force strict ordering of the ownership flag, in the event that we are already transmitting
	desc->flags |= flags::tx::first_segment(true);
	desc->flags |= flags::tx::last_segment(true);
	desc->flags |= flags::tx::frame_length(actual_size);
	__sync_synchronize();
	desc->flags |= flags::tx::ownership(flags::tx::owner_nic);
	__sync_synchronize();
	--_amount_free_descriptors;

	// TOOD: Technically, we should not be ringing the doorbell after every transmision, but only if there
	//       are no current transmissions in progress.
	//       Howver, always ringing the doorbell seems to work, and avoids the problem of having to detect
	//       stalls caused by not ringing the doorbell when it should have been rung
	nic.ringDoorbell();
	if(logTXDescriptor) {
		std::cout << "drivers/rtl8168: posting TX descriptor: "
			<< "tx_index: " << tx_index
			<< ", hw_tx_index: " << hw_tx_index
			<< ", amount_free_descriptors: " << _amount_free_descriptors
			<< std::endl;

		std::cout << "drivers/rtl8168: "
			<< "payload size: " << payload.size()
			<< ", actual_size: " << actual_size
			<< std::endl;
	}
	++tx_index;
	co_return;
}

// TODO: potential race conditions?
// TODO: locking the queue indexes might be a good idea
void TxQueue::handleTxOk() {
	while(hw_tx_index != tx_index) {
		auto i = hw_tx_index();
		if((_descriptors[i].flags & flags::tx::ownership) == flags::tx::owner_nic) // Descriptor was not transmitted?
			break;

		// Remove all info except EOR
		_descriptors[i].flags = flags::tx::eor(_descriptors[i].flags & flags::tx::eor);
		_descriptors[i].vlan = 0;

		if(_requests.empty())
			break;

		auto req = _requests.front();

		req->event.raise();
		_requests.pop();
		++hw_tx_index;
		++_amount_free_descriptors;
	}
	if(logIRQs) {
		std::cout << "drivers/rtl8168: completed handling of TX_OK"
			<< ", hw_tx_index: " << hw_tx_index()
			<< ", tx_index: " << tx_index()
			<< std::endl;
	}
}
