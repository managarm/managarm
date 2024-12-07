#include <set>
#include <core/tty.hpp>

#include "../controller.hpp"
#include "cp2102.hpp"

std::set<std::pair<std::string, std::string>> devices = {
	{"10c4", "ea60"},
};

namespace {

std::set<std::pair<size_t, size_t>> an205Table1 = {
	{ 300, 300 },
	{ 600, 600 },
	{ 1200, 1200 },
	{ 1800, 1800 },
	{ 2400, 2400 },
	{ 4000, 4000 },
	{ 4800, 4803 },
	{ 7200, 7207 },
	{ 9600, 9612 },
	{ 14400, 14428 },
	{ 16000, 16062 },
	{ 19200, 19250 },
	{ 28800, 28912 },
	{ 38400, 38601 },
	{ 51200, 51558 },
	{ 56000, 56280 },
	{ 57600, 58053 },
	{ 64000, 64111 },
	{ 76800, 77608 },
	{ 115200, 117028 },
	{ 128000, 129347 },
	{ 153600, 156868 },
	{ 230400, 237832 },
	{ 250000, 254234 },
	{ 256000, 273066 },
	{ 460800, 491520 },
	{ 500000, 567138 },
	{ 576000, 670254 },
	{ 921600, UINT_MAX },
};

size_t getAn205Rate(size_t baud) {
	auto res = std::find_if(an205Table1.begin(), an205Table1.end(), [&] (const auto &ref) {
		return baud <= ref.first;
	});

	if(res != an205Table1.end())
		return res->second;
	else
		return UINT_MAX;
}

}

bool Cp2102::valid(std::string vendor, std::string product) {
	return devices.contains({vendor, product});
}

async::result<void> Cp2102::initialize() {
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

	arch::dma_buffer_view buf{nullptr, &partnum_, sizeof(partnum_)};
	(co_await transferControl(hw(), pool_, true, uint8_t(cp2102::Request::VENDOR_SPECIFIC),
		uint16_t(cp2102::VendorRequest::GET_PARTNUM), intfNumber_, buf)).unwrap();

	std::cout << "usb-serial: CP2102 partnum " << uint8_t(partnum_) << std::endl;

	switch(partnum_) {
		case Partnum::CP2102:
			maxSpeed_ = 1000000;
			break;
		default:
			assert(!"unsupported partnum");
	}

	uint16_t control = 0x303;
	(co_await transferControl(hw(), pool_, false, uint8_t(cp2102::Request::SET_MHS), control,
		cp2102::CONFIG_INTERFACE, {})).unwrap();

	co_await setConfiguration(activeSettings);
}

async::result<void> Cp2102::setConfiguration(struct termios &new_config) {
	if(new_config.c_cflag & CBAUD) {
		size_t termios_baud = ttyConvertSpeed(cfgetospeed(&new_config));

		if(termios_baud) {
			uint32_t baud = getAn205Rate(termios_baud);
			(co_await transferControl(hw(), pool_, false, uint8_t(cp2102::Request::SET_BAUDRATE),
				0, 0, arch::dma_buffer_view{nullptr, &baud, sizeof(baud)})).unwrap();
		}
	}

	if(partnum_ == Partnum::CP2101) {
		// CP2101 only supports CS8, 1 stop bit and non-stick parity
		new_config.c_cflag &= ~(CSIZE | CSTOPB | CMSPAR);
		new_config.c_cflag |= CS8;
	}

	uint16_t bits = 0;

	switch(new_config.c_cflag & CSIZE) {
		case CS5:
			bits |= 0x500;
			break;
		case CS6:
			bits |= 0x600;
			break;
		case CS7:
			bits |= 0x700;
			break;
		case CS8:
		default:
			bits |= 0x800;
			break;
	}

	if(new_config.c_cflag & PARENB) {
		if(new_config.c_cflag & CMSPAR) {
			if(new_config.c_cflag & PARODD)
				bits |= 0x30;
			else
				bits |= 0x40;
		} else {
			if(new_config.c_cflag & PARODD)
				bits |= 0x10;
			else
				bits |= 0x20;
		}
	}

	if(new_config.c_cflag & CSTOPB)
		bits |= 2;

	(co_await transferControl(hw(), pool_, false, uint8_t(cp2102::Request::SET_LINE_CTL), bits, 0, {})).unwrap();

	if(cfgetospeed(&new_config) != 0 &&
		cfgetospeed(&activeSettings) != 0 &&
		(new_config.c_cflag & CRTSCTS) == (activeSettings.c_cflag & CRTSCTS) &&
		(new_config.c_iflag & IXON) == (activeSettings.c_iflag & IXON) &&
		(new_config.c_iflag & IXOFF) == (activeSettings.c_iflag & IXOFF) &&
		(new_config.c_cc[VSTART]) == (activeSettings.c_cc[VSTART]) &&
		(new_config.c_cc[VSTOP]) == (activeSettings.c_cc[VSTOP])
	) {
		goto finalize;
	}

	if(new_config.c_iflag & IXON || new_config.c_iflag & IXOFF) {
		cp2102::specialChars chars{};
		chars.bXonChar = new_config.c_cc[VSTART];
		chars.bXoffChar = new_config.c_cc[VSTOP];

		(co_await transferControl(hw(), pool_, false, uint8_t(cp2102::Request::SET_CHARS), 0,
			cp2102::CONFIG_INTERFACE, arch::dma_buffer_view{nullptr, &chars, sizeof(chars)})).unwrap();
	}

finalize:
	ttyCopyTermios(new_config, activeSettings);
}

async::result<protocols::usb::UsbError> Cp2102::send(protocols::usb::BulkTransfer transfer) {
	co_return (co_await out_->transfer(std::move(transfer))).maybe_error();
}

size_t Cp2102::sendFifoSize() {
	return cp2102::BULK_BUF_SIZE;
}
