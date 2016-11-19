
#include <memory>
#include <cofiber.hpp>
#include <cofiber/future.hpp>

struct DeviceState;
struct EndpointState;
struct ConfigurationState;
struct InterfaceState;
struct Controller;

enum XferFlags {
	kXferToDevice = 1,
	kXferToHost = 2,
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
	InterruptTransfer(void *buffer, size_t length);

	void *buffer;
	size_t length;
};

enum class PipeType {
	null, in, out, control
};

struct Endpoint {
	Endpoint(std::shared_ptr<EndpointState> state);
	
	cofiber::future<void> transfer(ControlTransfer info) const;
	cofiber::future<void> transfer(InterruptTransfer info) const;

private:
	std::shared_ptr<EndpointState> _state;
};

struct Interface {
	Interface(std::shared_ptr<InterfaceState> state);
	
	Endpoint getEndpoint(PipeType type, int number) const;

private:
	std::shared_ptr<InterfaceState> _state;
};

struct Configuration {
	Configuration(std::shared_ptr<ConfigurationState> state);
	
	cofiber::future<Interface> useInterface(int number, int alternative) const;

private:
	std::shared_ptr<ConfigurationState> _state;
};

struct Device {
	Device(std::shared_ptr<DeviceState> state);

	cofiber::future<std::string> configurationDescriptor() const;
	cofiber::future<Configuration> useConfiguration(int number) const;
	cofiber::future<void> transfer(ControlTransfer info) const;

private:
	std::shared_ptr<DeviceState> _state;
};

