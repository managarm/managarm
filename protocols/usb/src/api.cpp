
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

async::result<frg::expected<UsbError, std::string>> Device::configurationDescriptor() const {
	return _state->configurationDescriptor();
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

async::result<frg::expected<UsbError, Configuration>> Device::useConfiguration(int number) const {
	return _state->useConfiguration(number);
}

async::result<frg::expected<UsbError>> Device::transfer(ControlTransfer info) const {
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
