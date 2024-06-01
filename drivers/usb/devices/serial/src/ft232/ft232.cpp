#include <core/tty.hpp>
#include <set>

#include "../controller.hpp"
#include "ft232.hpp"

const std::set<std::pair<std::string, std::string>> devices = {
	{"0403", "6001"},
};

bool Ft232::valid(std::string vendor, std::string product) {
	return devices.contains({vendor, product});
}

async::result<void> Ft232::initialize() {
	auto deviceDescriptor = co_await hw().deviceDescriptor();
	assert(deviceDescriptor);
	auto desc = reinterpret_cast<protocols::usb::DeviceDescriptor *>(deviceDescriptor.value().data());

	switch(desc->bcdDevice) {
		case 0x400:
			type_ = Type::FT232B;
			break;
		case 0x600:
			type_ = Type::FT232R;
			break;
		default:
			printf("usb-serial: FTDI FT232 bcdDevice 0x%04x is unsupported\n", desc->bcdDevice);
			assert(!"unsupported");
	}

	auto descriptorOrError = co_await hw().configurationDescriptor();
	assert(descriptorOrError);

	std::optional<int> config_number;
	std::optional<int> in_endp_number;
	std::optional<int> out_endp_number;

	protocols::usb::walkConfiguration(descriptorOrError.value(), [&] (int type, size_t, void *descriptor, const auto &info) {
		if(type == protocols::usb::descriptor_type::configuration) {
			assert(!config_number);
			config_number = info.configNumber.value();
		} else if(type == protocols::usb::descriptor_type::interface) {
			intfNumber_ = info.interfaceNumber.value();
		} else if(type == protocols::usb::descriptor_type::endpoint) {
			auto desc = reinterpret_cast<protocols::usb::EndpointDescriptor *>(descriptor);

			if(info.endpointIn.value()) {
				in_endp_number = info.endpointNumber.value();
			} else {
				out_endp_number = info.endpointNumber.value();
				outMaxPacketSize_ = desc->maxPacketSize;
			}
		}
	});

	auto config = (co_await hw().useConfiguration(*config_number)).unwrap();
	if_ = (co_await config.useInterface(intfNumber_.value(), 0)).unwrap();
	in_ = (co_await if_->getEndpoint(protocols::usb::PipeType::in, in_endp_number.value())).unwrap();
	out_ = (co_await if_->getEndpoint(protocols::usb::PipeType::out, out_endp_number.value())).unwrap();

	co_await setConfiguration(activeSettings);
}

std::optional<uint32_t> Ft232::encodeBaud(size_t baud) {
	std::array<const uint8_t, 8> encoded_fraction = {0, 3, 2, 4, 1, 5, 6, 7};
	uint32_t clk = 3000000;

	if(baud < (clk >> 14) || baud > clk)
		return std::nullopt;

	uint32_t divisor = (clk << 4) / baud;

	if((divisor & 0xF) == 1)
		divisor &= ~7U;
	else
		divisor += 1;

	divisor >>= 1;

	uint32_t frac = divisor & 0x7;
	divisor >>= 3;

	if(divisor == 1) {
		if(frac == 0)
			divisor = 0;
		else
			frac = 0;
	}

	return (encoded_fraction.at(frac) << 14) | divisor;
}

async::result<void> Ft232::setConfiguration(struct termios &new_config) {
	size_t termios_baud = ttyConvertSpeed(cfgetospeed(&new_config));
	auto baud_setting = *encodeBaud(termios_baud);

	uint16_t lcr = 0;
	uint8_t v_start = 0;
	uint8_t v_stop = 0;
	uint8_t v_flow = 0;

	if (new_config.c_cflag & CSTOPB)
		lcr = uint16_t(ft232::StopBits::Bits2);
	else
		lcr = uint16_t(ft232::StopBits::Bits1);

	if (new_config.c_cflag & PARENB) {
		if (new_config.c_cflag & PARODD) {
			lcr |= uint16_t(ft232::Parity::Odd);
		} else {
			lcr |= uint16_t(ft232::Parity::Even);
		}
	} else {
		lcr |= uint16_t(ft232::Parity::None);
	}

	switch (new_config.c_cflag & CSIZE) {
	case CS5:
		lcr |= 5;
		break;

	case CS6:
		lcr |= 6;
		break;

	case CS7:
		lcr |= 7;
		break;

	case CS8:
		lcr |= 8;
		break;
	}

	if (new_config.c_cflag & CRTSCTS) {
		v_flow = uint8_t(ft232::FlowControl::Rts_Cts_Handshake);
	} else if (new_config.c_iflag & (IXON | IXOFF)) {
		v_flow = uint8_t(ft232::FlowControl::Xon_Xoff_Handshake);
		v_start = new_config.c_cc[VSTART];
		v_stop = new_config.c_cc[VSTOP];
	} else {
		v_flow = uint8_t(ft232::FlowControl::Disable);
	}

	co_await transferControl(hw_, pool_, false, uint8_t(ft232::Request::SetBaudRate), baud_setting & 0xFFFF, baud_setting >> 16, {});
	co_await transferControl(hw_, pool_, false, uint8_t(ft232::Request::SetData), lcr, 0, {});
	co_await transferControl(hw_, pool_, false, uint8_t(ft232::Request::SetFlowControl), v_stop | (v_start << 8), v_flow, {});

	ttyCopyTermios(new_config, activeSettings);

	co_return;
}

async::result<protocols::usb::UsbError> Ft232::send(protocols::usb::BulkTransfer transfer) {
	co_return (co_await out_->transfer(std::move(transfer))).maybe_error();
}

size_t Ft232::sendFifoSize() {
	return outMaxPacketSize_;
}
