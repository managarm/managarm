#include <format>
#include <set>
#include <core/tty.hpp>

#include "ch341.hpp"

namespace {

std::set<std::pair<std::string, std::string>> devices = {
	{"4348", "5523"},
	{"1a86", "5523"},
	{"1a86", "7522"},
	{"1a86", "7523"},
};

constexpr size_t CLKRATE = 48000000;

uint16_t getDivisor(size_t baud) {
	assert(baud);

	auto clk_div = [](size_t prescaler, size_t fact) {
		return 1 << (12 - 3 * prescaler - fact);
	};

	auto div_round_up = [](auto x, auto y) {
		return 1 + ((x - 1) / y);
	};

	auto speed = std::clamp(baud, div_round_up(CLKRATE, clk_div(0, 0) * 256), (CLKRATE / clk_div(3, 0) * 2));

	size_t fact = 1;
	size_t prescaler = 3;

	while(prescaler --> 0) {
		if(speed > div_round_up(CLKRATE, clk_div(prescaler, 1) * 512))
			break;
	}

	assert(prescaler >= 0);

	size_t clock_div = clk_div(prescaler, fact);
	size_t div = CLKRATE / (clock_div * speed);

	// TODO: handle broken devices with limited prescalers

	if(div < 9 || div > 255) {
		div /= 2;
		clock_div *= 2;
		fact = 0;
	}

	assert(div >= 2);

	if(16 * CLKRATE / (clock_div * div) - 16 * speed >= 16 * speed - 16 * CLKRATE / (clock_div * (div + 1)))
		div++;

	if(fact == 1 && div % 2 == 0) {
		div /= 2;
		fact = 0;
	}

	return (0x100 - div) << 8 | fact << 2 | prescaler;
}

} // namespace

bool Ch341::valid(std::string vendor, std::string product) {
	return devices.contains({vendor, product});
}

async::result<void> Ch341::initialize() {
	auto descriptorOrError = co_await hw().configurationDescriptor(0);
	assert(descriptorOrError);

	std::optional<int> config_number;
	std::optional<int> in_endp_number;
	std::optional<int> out_endp_number;

	protocols::usb::walkConfiguration(descriptorOrError.value(), [&] (int type, size_t, void *, const auto &info) {
		if(type == protocols::usb::descriptor_type::configuration) {
			assert(!config_number);
			config_number = info.configNumber.value();
		} else if(type == protocols::usb::descriptor_type::interface) {
			intfNumber_ = info.interfaceNumber.value();
		} else if(type == protocols::usb::descriptor_type::endpoint) {
			if(info.endpointIn.value()) {
				in_endp_number = info.endpointNumber.value();
			} else {
				out_endp_number = info.endpointNumber.value();
			}
		}
	});

	auto config = (co_await hw().useConfiguration(0, *config_number)).unwrap();
	if_ = (co_await config.useInterface(intfNumber_, 0)).unwrap();
	in_ = (co_await if_->getEndpoint(protocols::usb::PipeType::in, in_endp_number.value())).unwrap();
	out_ = (co_await if_->getEndpoint(protocols::usb::PipeType::out, out_endp_number.value())).unwrap();

	uint8_t buf[2];
	(co_await transferControl(hw_, pool_, true, static_cast<uint8_t>(ch341::Request::GetVersion), 0, 0, arch::dma_buffer_view{nullptr, buf, sizeof(buf)})).unwrap();

	version_ = buf[0];
	std::cout << std::format("usb-serial: CH341 version 0x{:x}\n", version_);

	(co_await transferControl(hw_, pool_, false, static_cast<uint8_t>(ch341::Request::SerialInit), 0, 0, {})).unwrap();

	memset(buf, 0, sizeof(buf));
	auto ret = co_await transferControl(hw_, pool_, true, static_cast<uint8_t>(ch341::Request::ReadReg), 0x05, 0, arch::dma_buffer_view{nullptr, buf, sizeof(buf)});
	if(!ret) {
		// the device is broken and needs workarounds :^)
		assert(!"usb-serial: CH341 device needs unimplemented quirks!");
	}

	co_await setConfiguration(activeSettings);
}

async::result<void> Ch341::setBaud(size_t baud, uint8_t bits) {
	auto val = getDivisor(baud);

	if(version_ > 0x27)
		val |= (1 << 7);

	(co_await transferControl(hw_, pool_, false, static_cast<uint8_t>(ch341::Request::WriteReg), 0x1312, val, {})).unwrap();

	if(version_ < 0x30)
		co_return;

	(co_await transferControl(hw_, pool_, false, static_cast<uint8_t>(ch341::Request::WriteReg), 0x2518, bits, {})).unwrap();

	co_return;
}

async::result<void> Ch341::setHandshake() {
	(co_await transferControl(hw_, pool_, false, static_cast<uint8_t>(ch341::Request::ModemCtrl), ~mcr_, 0, {})).unwrap();
}

async::result<void> Ch341::setConfiguration(struct termios &new_config) {
	uint8_t bits = 0x80 | 0x40;

	switch(new_config.c_cflag & CSIZE) {
		case CS5:
			bits |= 0x00;
			break;
		case CS6:
			bits |= 0x01;
			break;
		case CS7:
			bits |= 0x02;
			break;
		case CS8:
		default:
			bits |= 0x03;
			break;
	}

	if(new_config.c_cflag & PARENB) {
		bits |= 0x08;
		if(new_config.c_cflag & PARODD)
			bits |= 0x10;
		if(new_config.c_cflag & CMSPAR)
			bits |= 0x20;
	}

	if(new_config.c_cflag & CSTOPB)
		bits |= 0x04;

	if(new_config.c_cflag & CBAUD) {
		size_t termios_baud = ttyConvertSpeed(cfgetospeed(&new_config));

		if(termios_baud) {
			std::cout << std::format("usb-serial/ch341: setting baud {}\n", termios_baud);
			co_await setBaud(termios_baud, bits);

			mcr_ &= ~0x60;
		} else if(cfgetospeed(&activeSettings) == B0) {
			mcr_ |= 0x60;
		}
	}

	co_await setHandshake();

	ttyCopyTermios(new_config, activeSettings);

	co_return;
}

async::result<protocols::usb::UsbError> Ch341::send(protocols::usb::BulkTransfer transfer) {
	co_return (co_await out_->transfer(std::move(transfer))).maybe_error();
}

size_t Ch341::sendFifoSize() {
	return ch341::BULK_BUF_SIZE;
}
