#include <nic/usb_net/usb_net.hpp>

#include "usb-net.hpp"
#include "usb-ecm.hpp"
#include "usb-ncm.hpp"

UsbNic::UsbNic(protocols::usb::Device hw_device, nic::MacAddress mac,
		protocols::usb::Interface ctrl_intf, protocols::usb::Endpoint ctrl_ep,
		protocols::usb::Interface data_intf, protocols::usb::Endpoint in, protocols::usb::Endpoint out)
	: nic::Link{1500, &dmaPool_}, device_{hw_device}, ctrl_intf_{ctrl_intf}, ctrl_ep_{ctrl_ep},
	data_intf_{data_intf}, data_in_{in}, data_out_{out} {
	mac_ = mac;
}

namespace nic::usb_net {

async::result<std::shared_ptr<nic::Link>> makeShared(protocols::usb::Device hw_device, MacAddress mac,
		ConfigurationInfo info) {
	auto config = (co_await hw_device.useConfiguration(*info.chosen_configuration)).unwrap();
	auto ctrl_intf = (co_await config.useInterface(*info.control_if, 0)).unwrap();
	auto ctrl_ep = (co_await ctrl_intf.getEndpoint(protocols::usb::PipeType::in, *info.int_endp_number)).value();

	auto data_intf = (co_await config.useInterface(*info.data_if, 1)).unwrap();
	auto data_in = (co_await data_intf.getEndpoint(protocols::usb::PipeType::in, info.in_endp_number.value())).unwrap();
	auto data_out = (co_await data_intf.getEndpoint(protocols::usb::PipeType::out, info.out_endp_number.value())).unwrap();

	if(info.ncm) {
		auto nic = std::make_shared<nic::usb_ncm::UsbNcmNic>(std::move(hw_device), mac, std::move(ctrl_intf), std::move(ctrl_ep),
			std::move(data_intf), std::move(data_in), std::move(data_out), info.configuration_index);
		co_await nic->initialize();

		co_return nic;
	} else {
		auto nic = std::make_shared<nic::usb_ecm::UsbEcmNic>(std::move(hw_device), mac, std::move(ctrl_intf), std::move(ctrl_ep),
			std::move(data_intf), std::move(data_in), std::move(data_out));
		co_await nic->initialize();

		co_return nic;
	}
}

} // namespace nic::usb_net
