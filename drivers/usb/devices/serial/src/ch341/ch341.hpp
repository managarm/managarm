#pragma once

#include <cstdint>
#include <cstddef>

#include "../controller.hpp"

struct Ch341 : Controller {
	Ch341(protocols::usb::Device hw) : Controller{std::move(hw)} {};

	async::result<void> initialize() override;
	async::result<protocols::usb::UsbError> send(protocols::usb::BulkTransfer transfer) override;
	async::result<void> setConfiguration(struct termios &new_config) override;

	size_t sendFifoSize() override;

	static bool valid(std::string vendor, std::string product);

private:
	async::result<void> setBaud(size_t baud, uint8_t bits);
	async::result<void> setHandshake();

	uint16_t intfNumber_;
	std::optional<protocols::usb::Interface> if_;
	std::optional<protocols::usb::Endpoint> int_;
	std::optional<protocols::usb::Endpoint> in_;
	std::optional<protocols::usb::Endpoint> out_;

	uint8_t version_ = 0;
	uint8_t mcr_ = 0;
};

namespace ch341 {

constexpr size_t BULK_BUF_SIZE = 1024;

enum class Request : uint8_t {
	GetVersion = 0x5F,
	ReadReg = 0x95,
	WriteReg = 0x9A,
	SerialInit = 0xA1,
	ModemCtrl = 0xA4,
};

} // namespace ch341
