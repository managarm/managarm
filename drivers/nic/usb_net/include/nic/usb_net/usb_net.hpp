#pragma once

#include <netserver/nic.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/client.hpp>

namespace nic::usb_net {

enum class RequestCode : uint8_t {
	SEND_ENCAPSULATED_COMMAND = 0x00,
	GET_ENCAPSULATED_RESPONSE = 0x01,
	SET_ETHERNET_MULTICAST_FILTERS = 0x40,
	SET_ETHERNET_POWER_MANAGEMENT_PATTERN_FILTER = 0x41,
	GET_ETHERNET_POWER_MANAGEMENT_PATTERN_FILTER = 0x42,
	SET_ETHERNET_PACKET_FILTER = 0x43,
	GET_ETHERNET_STATISTIC = 0x44,
	GET_NTB_PARAMETERS = 0x80,
	GET_NET_ADDRESS = 0x81,
	SET_NET_ADDRESS = 0x82,
	GET_NTB_FORMAT = 0x83,
	SET_NTB_FORMAT = 0x84,
	GET_NTB_INPUT_SIZE = 0x85,
	SET_NTB_INPUT_SIZE = 0x86,
	GET_MAX_DATAGRAM_SIZE = 0x87,
	SET_MAX_DATAGRAM_SIZE = 0x88,
	GET_CRC_MODE = 0x89,
	SET_CRC_MODE = 0x8A,
};

struct ConfigurationInfo {
	bool ncm = false;
	bool valid = false;

	uint8_t subclass = protocols::usb::cdc_subclass::reserved;

	size_t configuration_index;

	std::optional<uint8_t> chosen_configuration;
	std::optional<uint8_t> iMACAddress;
	std::optional<uint8_t> control_if;
	std::optional<uint8_t> data_if;

	std::optional<int> int_endp_number;
	std::optional<int> in_endp_number;
	std::optional<int> out_endp_number;
};

async::result<std::shared_ptr<nic::Link>> makeShared(
    mbus_ng::EntityId entity,
    protocols::usb::Device hw_device,
    MacAddress mac,
    ConfigurationInfo info
);

} // namespace nic::usb_net
