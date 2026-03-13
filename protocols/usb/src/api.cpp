#include <string>
#include <locale>
#include <codecvt>
#include <print>

#include "protocols/usb/api.hpp"

namespace protocols::usb {

// ----------------------------------------------------------------------------
// Device.
// ----------------------------------------------------------------------------

Device::Device(std::shared_ptr<DeviceData> state)
: _state(std::move(state)) { }

arch::dma_pool *Device::setupPool() const {
	return _state->setupPool();
}

arch::dma_pool *Device::bufferPool() const {
	return _state->bufferPool();
}

async::result<frg::expected<UsbError, std::string>> Device::deviceDescriptor() const {
	return _state->deviceDescriptor();
}

async::result<frg::expected<UsbError, std::string>> Device::configurationDescriptor(uint8_t configuration) const {
	return _state->configurationDescriptor(configuration);
}

async::result<frg::expected<UsbError, uint8_t>> Device::currentConfigurationValue() const {
	arch::dma_object<SetupPacket> get{setupPool()};
	get->type = setup_type::targetDevice | setup_type::byStandard
			| setup_type::toHost;
	get->request = request_type::getConfig;
	get->value = 0;
	get->index = 0;
	get->length = 1;

	arch::dma_object<uint8_t> descriptor{bufferPool()};
	FRG_CO_TRY(co_await transfer(ControlTransfer{kXferToHost,
			get, descriptor.view_buffer()}));

	co_return *descriptor.data();
}

async::result<frg::expected<UsbError, Configuration>> Device::useConfiguration(uint8_t index, uint8_t value) const {
	return _state->useConfiguration(index, value);
}


void hexDump(const void *buf, size_t size) {
	auto ptr = reinterpret_cast<const uint8_t *>(buf);

	std::println("hexDump({:016x}, {}) called", reinterpret_cast<uintptr_t>(buf), size);
	
	for (size_t i = 0; i < size; i += 16) {
		size_t chunk = std::min(size_t{16}, size - i);

		std::print("{:016x} | ", reinterpret_cast<uintptr_t>(buf) + i);

		for (size_t j = 0; j < 16; j++) {
			if (j >= chunk) {
				std::print("   ");
			} else {
				std::print("{:02x} ", ptr[i + j]);
			}
		}

		std::print("| ");

		for (size_t j = 0; j < chunk; j++) {
			std::print("{:c} ", std::isprint(ptr[i + j]) ? ptr[i + j] : '.');
		}

		std::println();
	}
}


async::result<frg::expected<UsbError, std::string>> Device::getString(size_t number) const {
	if(number == 0)
		co_return UsbError::unsupported;

	arch::dma_object<SetupPacket> desc{setupPool()};
	desc->type = setup_type::targetDevice | setup_type::byStandard | setup_type::toHost;
	desc->request = request_type::getDescriptor;
	desc->value = (descriptor_type::string << 8) | number;
	desc->index = 0x0409; // en-us
	desc->length = sizeof(StringDescriptor);

	arch::dma_object<StringDescriptor> header{bufferPool()};

	FRG_CO_TRY(co_await transfer(ControlTransfer(kXferToHost, desc, header.view_buffer())));

	desc->length = header->length;
	arch::dma_buffer buffer{bufferPool(), header->length};
	FRG_CO_TRY(co_await transfer(ControlTransfer(kXferToHost, desc, buffer)));

	std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t> convert;
	auto res = reinterpret_cast<StringDescriptor *>(buffer.data());

	hexDump(res->data, res->length);
	
	co_return convert.to_bytes(std::u16string{res->data, (res->length - sizeof(StringDescriptor)) / 2});
}

async::result<frg::expected<UsbError, size_t>> Device::transfer(ControlTransfer info) const {
	return _state->transfer(info);
}

// ----------------------------------------------------------------------------
// Configuration.
// ----------------------------------------------------------------------------

Configuration::Configuration(std::shared_ptr<ConfigurationData> state)
: _state(std::move(state)) { }

async::result<frg::expected<UsbError, Interface>> Configuration::useInterface(int number,
		int alternative) const {
	return _state->useInterface(number, alternative);
}

// ----------------------------------------------------------------------------
// Interface.
// ----------------------------------------------------------------------------

Interface::Interface(std::shared_ptr<InterfaceData> state)
: _state(std::move(state)) { }

async::result<frg::expected<UsbError, Endpoint>>
Interface::getEndpoint(PipeType type, int number) const {
	return _state->getEndpoint(type, number);
}

// ----------------------------------------------------------------------------
// Endpoint.
// ----------------------------------------------------------------------------

Endpoint::Endpoint(std::shared_ptr<EndpointData> state)
: _state(std::move(state)) { }

async::result<frg::expected<UsbError, size_t>> Endpoint::transfer(InterruptTransfer info) const {
	return _state->transfer(info);
}

async::result<frg::expected<UsbError, size_t>> Endpoint::transfer(BulkTransfer info) const {
	return _state->transfer(info);
}

} // namespace protocols::usb
