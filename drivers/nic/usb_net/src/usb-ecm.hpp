#pragma once

#include "usb-net.hpp"

namespace nic::usb_ecm {

struct UsbEcmNic : UsbNic {
	UsbEcmNic(protocols::usb::Device hw_device, nic::MacAddress mac,
		protocols::usb::Interface ctrl_intf, protocols::usb::Endpoint ctrl_ep,
		protocols::usb::Interface intf, protocols::usb::Endpoint in, protocols::usb::Endpoint out);

	async::result<void> initialize() override;
	async::detached listenForNotifications() override;

	async::result<size_t> receive(arch::dma_buffer_view) override;
	async::result<void> send(const arch::dma_buffer_view) override;
};

} // namespace nic::usb_ecm
