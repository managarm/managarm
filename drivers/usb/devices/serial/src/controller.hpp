#pragma once

#include <arch/dma_pool.hpp>
#include <async/oneshot-event.hpp>
#include <smarter.hpp>
#include <protocols/usb/client.hpp>
#include <span>
#include <termios.h>
#include <unistd.h>

namespace {

[[maybe_unused]]
async::result<frg::expected<protocols::usb::UsbError, size_t>> transferControl(protocols::usb::Device &device, arch::contiguous_pool &pool,
		bool read, uint8_t request, uint16_t value, uint16_t interface, arch::dma_buffer_view buf) {
	arch::dma_object<protocols::usb::SetupPacket> ctrl_msg{&pool};
	ctrl_msg->type = protocols::usb::setup_type::byVendor |
					(read ? protocols::usb::setup_type::toHost : protocols::usb::setup_type::toDevice);
	ctrl_msg->request = uint8_t(request);
	ctrl_msg->value = value;
	ctrl_msg->index = interface;
	ctrl_msg->length = buf.size();

	co_return co_await device.transfer(
		protocols::usb::ControlTransfer{(read ? protocols::usb::kXferToHost : protocols::usb::kXferToDevice),
		ctrl_msg, buf});
}

}

struct Controller {
	Controller(protocols::usb::Device hw);

	virtual async::result<void> initialize() = 0;
	virtual async::result<protocols::usb::UsbError> send(protocols::usb::BulkTransfer transfer) = 0;

	virtual async::result<void> setConfiguration(struct termios &new_config) = 0;

	virtual size_t sendFifoSize() = 0;

	async::detached flushSends();

	protocols::usb::Device &hw() {
		return hw_;
	}

	struct termios activeSettings = {};
	bool nonBlock_;
	protocols::usb::Device hw_;
	arch::contiguous_pool pool_;
};

struct WriteRequest {
	WriteRequest(std::span<const uint8_t> buffer, Controller *controller)
	: buffer(buffer), progress(0), controller{controller} { }

	std::span<const uint8_t> buffer;
	size_t progress;
	async::oneshot_event event;
	frg::default_list_hook<WriteRequest> hook;

	Controller *controller;
};
