
#include <memory>
#include <cofiber.hpp>
#include <cofiber/future.hpp>

struct DeviceState;
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
	Endpoint(std::shared_ptr<Controller> controller, std::shared_ptr<DeviceState> device_state,
			PipeType type, int number);
	
	cofiber::future<void> transfer(ControlTransfer info);
	cofiber::future<void> transfer(InterruptTransfer info);

private:
	std::shared_ptr<Controller> _controller;
	std::shared_ptr<DeviceState> _deviceState;
	PipeType _type;
	int _number;
};

struct Interface {
	Interface(std::shared_ptr<Controller> controller, std::shared_ptr<DeviceState> device_state);
	
	Endpoint getEndpoint(PipeType type, int number);

private:
	std::shared_ptr<Controller> _controller;
	std::shared_ptr<DeviceState> _deviceState;
};

struct Configuration {
	Configuration(std::shared_ptr<Controller> controller, std::shared_ptr<DeviceState> device_state);
	
	cofiber::future<Interface> useInterface(int number, int alternative) const;

private:
	std::shared_ptr<Controller> _controller;
	std::shared_ptr<DeviceState> _deviceState;
};

struct Device {
	Device(std::shared_ptr<Controller> controller, std::shared_ptr<DeviceState> device_state);

	cofiber::future<std::string> configurationDescriptor() const;
	cofiber::future<Configuration> useConfiguration(int number) const;
	cofiber::future<void> transfer(ControlTransfer info) const;

private:
	std::shared_ptr<Controller> _controller;
	std::shared_ptr<DeviceState> _deviceState;
};

