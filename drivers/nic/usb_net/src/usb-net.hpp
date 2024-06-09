#pragma once

#include <netserver/nic.hpp>
#include <protocols/usb/client.hpp>

struct UsbNic : nic::Link {
	UsbNic(protocols::usb::Device hw_device, nic::MacAddress mac,
		protocols::usb::Interface ctrl_intf, protocols::usb::Endpoint ctrl_ep,
		protocols::usb::Interface intf, protocols::usb::Endpoint in, protocols::usb::Endpoint out);

	virtual async::result<void> initialize() = 0;

	~UsbNic() override = default;
protected:
	virtual async::detached listenForNotifications() = 0;

	arch::contiguous_pool dmaPool_;
	protocols::usb::Device device_;

	/* the control interface */
	protocols::usb::Interface ctrl_intf_;
	/* interrupt in endpoint */
	protocols::usb::Endpoint ctrl_ep_;
	/* the data interface */
	protocols::usb::Interface data_intf_;
	/* data in endpoint */
	protocols::usb::Endpoint data_in_;
	/* data out endpoint */
	protocols::usb::Endpoint data_out_;

	bool ncm_;

	uint16_t seq_ = 0;
};
