#include <async/basic.hpp>
#include <nic/rtl8168/common.hpp>
#include <nic/rtl8168/debug_options.hpp>
#include <nic/rtl8168/descriptor.hpp>
#include <nic/rtl8168/regs.hpp>
#include <stdio.h>
#include <string.h>

void RealtekNic::setRxConfigRegisters() {
	switch (_revision) {
	case MacRevision::MacVer02... MacRevision::MacVer06:
	case MacRevision::MacVer10... MacRevision::MacVer17: {
		_mmio.store(
		    regs::receive_config,
		    flags::receive_config::rxfth(flags::receive_config::rxfth_none) |
		        flags::receive_config::mxdma(flags::receive_config::mxdma_unlimited) |
		        flags::receive_config::accept_packet_with_destination_addr(false) |
		        flags::receive_config::accept_packet_with_physical_match(true) |
		        flags::receive_config::accept_multicast_packets(true) |
		        flags::receive_config::accept_broadcast_packets(true)
		);
		break;
	}
	case MacRevision::MacVer18... MacRevision::MacVer24:
	case MacRevision::MacVer34... MacRevision::MacVer36:
	case MacRevision::MacVer38: {
		_mmio.store(
		    regs::receive_config,
		    flags::receive_config::rx128_int_en(true) | flags::receive_config::rx_multi_en(true) |
		        flags::receive_config::mxdma(flags::receive_config::mxdma_unlimited) |
		        flags::receive_config::accept_packet_with_destination_addr(false) |
		        flags::receive_config::accept_packet_with_physical_match(true) |
		        flags::receive_config::accept_multicast_packets(true) |
		        flags::receive_config::accept_broadcast_packets(true)
		);
		break;
	}
	case MacRevision::MacVer40... MacRevision::MacVer53: {
		_mmio.store(
		    regs::receive_config,
		    flags::receive_config::rx128_int_en(true) | flags::receive_config::rx_multi_en(true) |
		        flags::receive_config::mxdma(flags::receive_config::mxdma_unlimited) |
		        flags::receive_config::rx_early_off(true) |
		        flags::receive_config::accept_packet_with_destination_addr(false) |
		        flags::receive_config::accept_packet_with_physical_match(true) |
		        flags::receive_config::accept_multicast_packets(true) |
		        flags::receive_config::accept_broadcast_packets(true)
		);
		break;
	}
	case MacRevision::MacVer61... MacRevision::MacVer63: {
		_mmio.store(
		    regs::receive_config,
		    flags::receive_config::rx_fetch(flags::receive_config::rx_fetch_default_8125) |
		        flags::receive_config::mxdma(flags::receive_config::mxdma_unlimited) |
		        flags::receive_config::accept_packet_with_destination_addr(false) |
		        flags::receive_config::accept_packet_with_physical_match(true) |
		        flags::receive_config::accept_multicast_packets(true) |
		        flags::receive_config::accept_broadcast_packets(true)
		);
		break;
	}
	default: {
		_mmio.store(
		    regs::receive_config,
		    flags::receive_config::rx128_int_en(true) |
		        flags::receive_config::mxdma(flags::receive_config::mxdma_unlimited) |
		        flags::receive_config::accept_packet_with_destination_addr(true) |
		        flags::receive_config::accept_packet_with_physical_match(true) |
		        flags::receive_config::accept_multicast_packets(true) |
		        flags::receive_config::accept_broadcast_packets(true)
		);
		break;
	}
	}

	_mmio.store(regs::rx_max_size, 0x1FFF);
}

void RealtekNic::closeRX() {
	_mmio.store(
	    regs::receive_config,
	    _mmio.load(regs::receive_config) / flags::receive_config::accept_mask_bits(0)
	);
}

RxQueue::RxQueue(size_t descriptors, RealtekNic &nic)
    : _descriptor_count{descriptors},
      _last_rx_index(0, descriptors),
      _next_index(0, descriptors) {
	_descriptors = arch::dma_array<Descriptor>(nic.dmaPool(), _descriptor_count);
	_descriptor_buffers.reserve(_descriptor_count);

	for (size_t i = 0; i < _descriptor_count; i++) {
		auto buf = arch::dma_buffer(nic.dmaPool(), 2048);
		memset(buf.data(), 0, buf.size());
		uintptr_t addr = helix_ng::ptrToPhysical(buf.data());

		_descriptor_buffers.push_back(std::move(buf));

		_descriptors[i].flags =
		    flags::rx::ownership(flags::rx::owner_nic) | flags::rx::frame_length(2048);
		_descriptors[i].vlan = 0;
		_descriptors[i].base_low = addr & 0xFFFF'FFFF;
		_descriptors[i].base_high = (addr >> 32) & 0xFFFF'FFFF;
	}

	_descriptors[_descriptor_count - 1].flags |= flags::rx::eor(true);
}

async::result<size_t> RxQueue::submitDescriptor(arch::dma_buffer_view frame, RealtekNic &nic) {
	auto ev_req = std::make_shared<Request>(_descriptor_count);

	co_await postDescriptor(frame, nic, ev_req);

	co_await ev_req->event.wait();

	co_return ev_req->frame.size();
}

async::result<void> RxQueue::postDescriptor(
    arch::dma_buffer_view frame, RealtekNic &nic, std::shared_ptr<Request> req
) {
	req->frame = frame;
	req->index = _next_index;

	_requests.push(std::move(req));

	++_next_index;

	co_return;
}

bool RxQueue::checkOwnerOfNextDescriptor() {
	return (_descriptors[_next_index].flags & flags::rx::ownership) == flags::rx::owner_nic;
}

// TODO: support large packets
void RxQueue::handleRxOk() {
	while (!_requests.empty()) {
		auto req = _requests.front();
		auto i = req->index;

		if ((_descriptors[i].flags & flags::rx::ownership) ==
		    flags::rx::owner_nic) // Descriptor was not transmitted?
			break;

		__sync_synchronize();

		auto _flags = _descriptors[i].flags;

		if (logRXDescriptor) {
			std::cout << "drivers/rtl8168: got RX descriptor, flags:" << std::endl;

			if (_flags & flags::rx::eor) {
				std::cout << "\t\t eor" << std::endl;
			}
			if (_flags & flags::rx::physical_address_ok) {
				std::cout << "\t\t physical_address_ok" << std::endl;
			}
			if (_flags & flags::rx::first_segment) {
				std::cout << "\t\t first_segment" << std::endl;
			}
			if (_flags & flags::rx::last_segment) {
				std::cout << "\t\t last_segment" << std::endl;
			}
			if (_flags & flags::rx::broadcast_packet) {
				std::cout << "\t\t broadcast_packet" << std::endl;
			}
			if (_flags & flags::rx::receive_watchdog_timer_expired) {
				std::cout << "\t\t receive_watchdog_timer_expired" << std::endl;
			}
			if (_flags & flags::rx::receive_error) {
				std::cout << "\t\t receive_error" << std::endl;
			}
		}

		auto size = _flags & flags::rx::frame_length;

		if (size == 0) {
			break;
		}

		memcpy(req->frame.data(), _descriptor_buffers[i].data(), size);
		req->frame = req->frame.subview(0, size);

		_descriptors[i].flags = flags::rx::eor(_descriptors[i].flags & flags::rx::eor) |
		                        flags::rx::ownership(flags::rx::owner_nic) |
		                        flags::rx::frame_length(2048);
		_descriptors[i].vlan = 0;

		req->event.raise();
		_requests.pop();
	}
}
