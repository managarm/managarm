
#ifndef LIBUSB_API_HPP
#define LIBUSB_API_HPP

#include <memory>

#include <async/result.hpp>
#include <cofiber.hpp>

#include "usb.hpp"

enum XferFlags {
	kXferToDevice = 1,
	kXferToHost = 2
};

struct ControlTransfer {
	ControlTransfer(XferFlags flags, ControlRecipient recipient, ControlType type, uint8_t request,
			uint16_t arg0, uint16_t arg1, void *buffer, size_t length);

	XferFlags flags;
	ControlRecipient recipient;
	ControlType type;
	uint8_t request;
	uint16_t arg0;
	uint16_t arg1;
	void *buffer;
	size_t length;
};

struct InterruptTransfer {
	InterruptTransfer(XferFlags flags, void *buffer, size_t length);

	XferFlags flags;
	void *buffer;
	size_t length;
};

enum class PipeType {
	null, in, out, control
};

// ----------------------------------------------------------------------------
// EndpointData
// ----------------------------------------------------------------------------

struct EndpointData {
	virtual async::result<void> transfer(ControlTransfer info) = 0;
	virtual async::result<void> transfer(InterruptTransfer info) = 0;
};


struct Endpoint {
	Endpoint(std::shared_ptr<EndpointData> state);
	
	async::result<void> transfer(ControlTransfer info) const;
	async::result<void> transfer(InterruptTransfer info) const;

private:
	std::shared_ptr<EndpointData> _state;
};

// ----------------------------------------------------------------------------
// InterfaceData
// ----------------------------------------------------------------------------

struct InterfaceData {
	virtual async::result<Endpoint> getEndpoint(PipeType type, int number) = 0;
};

struct Interface {
	Interface(std::shared_ptr<InterfaceData> state);
	
	async::result<Endpoint> getEndpoint(PipeType type, int number) const;

private:
	std::shared_ptr<InterfaceData> _state;
};


// ----------------------------------------------------------------------------
// ConfigurationData
// ----------------------------------------------------------------------------

struct ConfigurationData {
	virtual async::result<Interface> useInterface(int number, int alternative) = 0;
};

struct Configuration {
	Configuration(std::shared_ptr<ConfigurationData> state);
	
	async::result<Interface> useInterface(int number, int alternative) const;

private:
	std::shared_ptr<ConfigurationData> _state;
};

// ----------------------------------------------------------------------------
// DeviceData
// ----------------------------------------------------------------------------

struct DeviceData {
	virtual async::result<std::string> configurationDescriptor() = 0;
	virtual async::result<Configuration> useConfiguration(int number) = 0;
	virtual async::result<void> transfer(ControlTransfer info) = 0;
};

struct Device {
	Device(std::shared_ptr<DeviceData> state);

	async::result<std::string> configurationDescriptor() const;
	async::result<Configuration> useConfiguration(int number) const;
	async::result<void> transfer(ControlTransfer info) const;

private:
	std::shared_ptr<DeviceData> _state;
};

#endif // LIBUSB_API_HPP

