
#include "protocols/usb/api.hpp"

// ----------------------------------------------------------------------------
// Device.
// ----------------------------------------------------------------------------

Device::Device(std::shared_ptr<DeviceData> state)
: _state(std::move(state)) { }

cofiber::future<std::string> Device::configurationDescriptor() const {
	return _state->configurationDescriptor();
}

cofiber::future<Configuration> Device::useConfiguration(int number) const {
	return _state->useConfiguration(number);
}

cofiber::future<void> Device::transfer(ControlTransfer info) const {
	return _state->transfer(info);
}

// ----------------------------------------------------------------------------
// Configuration.
// ----------------------------------------------------------------------------

Configuration::Configuration(std::shared_ptr<ConfigurationData> state)
: _state(std::move(state)) { }

cofiber::future<Interface> Configuration::useInterface(int number,
		int alternative) const {
	return _state->useInterface(number, alternative);
}

// ----------------------------------------------------------------------------
// Interface.
// ----------------------------------------------------------------------------

Interface::Interface(std::shared_ptr<InterfaceData> state)
: _state(std::move(state)) { }

Endpoint Interface::getEndpoint(PipeType type, int number) const {
	return _state->getEndpoint(type, number);
}

// ----------------------------------------------------------------------------
// Endpoint.
// ----------------------------------------------------------------------------

Endpoint::Endpoint(std::shared_ptr<EndpointData> state)
: _state(std::move(state)) { }

cofiber::future<void> Endpoint::transfer(InterruptTransfer info) const {
	return _state->transfer(info);
}

