#ifndef POSIX_SUBSYSTEM_DRVCORE_HPP
#define POSIX_SUBSYSTEM_DRVCORE_HPP

#include <string>
#include <sstream>

#include "device.hpp"
#include "sysfs.hpp"

namespace drvcore {

// This struct corresponds to Linux' struct Device (i.e. a device that is part of sysfs).
// TODO: Make the sysfs::Object private?
struct Device : sysfs::Object {
	Device(std::shared_ptr<Device> parent, std::string name, UnixDevice *unix_device);

protected:
	~Device() = default;

public:
	void setupDevicePtr(std::weak_ptr<Device> self) {
		_devicePtr = std::move(self);
	}

	std::shared_ptr<Device> devicePtr() {
		return std::shared_ptr<Device>(_devicePtr);
	}

	UnixDevice *unixDevice() {
		return _unixDevice;
	}

	virtual void linkToSubsystem();

	virtual void composeUevent(std::stringstream &ss) = 0;

private:
	std::weak_ptr<Device> _devicePtr;
	UnixDevice *_unixDevice;
};

struct ClassSubsystem {
	ClassSubsystem(std::string name);

	std::shared_ptr<sysfs::Object> object() {
		return _object;
	}

private:
	std::shared_ptr<sysfs::Object> _object;
};

struct ClassDevice : Device {
	ClassDevice(ClassSubsystem *subsystem, std::string name,
			UnixDevice *unix_device);

	void linkToSubsystem() override;

private:
	ClassSubsystem *_subsystem;
};

void initialize();

void installDevice(std::shared_ptr<Device> device);

uint32_t makeHotplugSeqnum();

void emitHotplug(std::string buffer);

} // namespace drvcore

#endif // POSIX_SUBSYSTEM_DRVCORE_HPP
