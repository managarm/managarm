#pragma once

#include <cstdint>
#include <cstddef>

#include "controller.hpp"

struct Cp2102 : Controller {
	Cp2102(protocols::usb::Device hw) : Controller{std::move(hw)} {};

	virtual async::result<void> initialize() override;
	virtual async::result<protocols::usb::UsbError> send(protocols::usb::BulkTransfer transfer) override;
	virtual async::result<void> setConfiguration(struct termios &new_config) override;

	virtual size_t sendFifoSize() override;

	static bool valid(std::string vendor, std::string product);

private:
	enum class Partnum : uint8_t {
		CP2101 = 1,
		CP2102 = 2,
		CP2103 = 3,
		CP2104 = 4,
		CP2105 = 5,
		CP2108 = 8,
	};

	Partnum partnum_;

	size_t maxSpeed_;

	uint16_t intfNumber_;
	std::optional<protocols::usb::Interface> if_;
	std::optional<protocols::usb::Endpoint> in_;
	std::optional<protocols::usb::Endpoint> out_;
};

namespace cp2102 {

constexpr size_t BULK_BUF_SIZE = 1024;
constexpr uint16_t CONFIG_INTERFACE = 0;
constexpr size_t BAUDDIV_REF = 3686400; /* 3.6864 MHz */

/* request codes */
enum class Request : uint8_t {
	IFC_ENABLE = 0x00,
	SET_BAUDDIV = 0x01,
	GET_BAUDDIV = 0x02,
	SET_LINE_CTL = 0x03,
	GET_LINE_CTL = 0x04,
	SET_BREAK = 0x05,
	IMM_CHAR = 0x06,
	SET_MHS = 0x07,
	GET_MDMSTS = 0x08,
	SET_XON = 0x09,
	SET_XOFF = 0x0A,
	SET_EVENTMASK = 0x0B,
	GET_EVENTMASK = 0x0C,
	SET_CHAR = 0xD,
	GET_CHARS = 0xE,
	GET_PROPS = 0xF,
	GET_COMM_STATUS = 0x10,
	RESET = 0x11,
	PURGE = 0x12,
	SET_FLOW = 0x13,
	GET_FLOW = 0x14,
	EMBED_EVENTS = 0x15,
	GET_EVENTSTATE = 0x16,
	SET_CHARS = 0x19,
	GET_BAUDRATE = 0x1D,
	SET_BAUDRATE = 0x1E,
	VENDOR_SPECIFIC = 0xFF,
};

enum class VendorRequest : uint16_t {
	GET_FW_VER = 0x000E,
	READ_LATCH = 0x00C2,
	GET_PARTNUM = 0x370B,
	GET_PORTCONFIG = 0x370C,
	GET_DEVICEMODE = 0x3711,
	WRITE_LATCH = 0x37E1,
};

struct specialChars {
	uint8_t bEofChar;
	uint8_t bErrorChar;
	uint8_t bBreakChar;
	uint8_t bEventChar;
	uint8_t bXonChar;
	uint8_t bXoffChar;
};

}
