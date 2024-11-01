#pragma once

#include <cstddef>
#include <cstdint>

#include "../controller.hpp"

struct Ft232 final : Controller {
	Ft232(protocols::usb::Device hw) : Controller{std::move(hw)} {};

	async::result<void> initialize() override;
	async::result<protocols::usb::UsbError> send(protocols::usb::BulkTransfer transfer) override;
	async::result<void> setConfiguration(struct termios &new_config) override;

	size_t sendFifoSize() override;

	static bool valid(std::string vendor, std::string product);

  private:
	std::optional<uint32_t> encodeBaud(size_t baud);

	enum class Type {
		FT232B,
		FT232R,
	};

	Type type_;

	std::optional<uint16_t> intfNumber_{};
	std::optional<protocols::usb::Interface> if_{};
	std::optional<protocols::usb::Endpoint> in_{};
	std::optional<protocols::usb::Endpoint> out_{};

	size_t outMaxPacketSize_ = 0;
};

namespace ft232 {

enum class Request : uint8_t {
	Reset = 0,
	SetFlowControl = 2,
	SetBaudRate = 3,
	SetData = 4,
};

enum class Parity : uint16_t {
	None = 0 << 8,
	Odd = 1 << 8,
	Even = 2 << 8,
	Mark = 3 << 8,
};

enum class StopBits : uint16_t {
	Bits1 = 0 << 11,
	Bits15 = 1 << 11,
	Bits2 = 2 << 11,
};

enum class FlowControl : uint8_t {
	Disable = 0,
	Rts_Cts_Handshake = 1,
	Dtr_Dsr_Handshake = 2,
	Xon_Xoff_Handshake = 4,
};

} // namespace ft232
