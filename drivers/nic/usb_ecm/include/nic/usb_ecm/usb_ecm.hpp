#pragma once

#include <netserver/nic.hpp>
#include <protocols/usb/client.hpp>

namespace nic::usb_ecm {

std::shared_ptr<nic::Link> makeShared(protocols::usb::Device hw_device, MacAddress mac, protocols::usb::Endpoint in, protocols::usb::Endpoint out);

} // namespace nic::usb_ecm
