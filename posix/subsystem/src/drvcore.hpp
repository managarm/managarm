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
	UnixDevice *unixDevice() {
		return _unixDevice;
	}

	virtual void composeUevent(std::stringstream &ss) = 0;

private:
	UnixDevice *_unixDevice;
};

void initialize();

void installDevice(std::shared_ptr<Device> device);

uint32_t makeHotplugSeqnum();

void emitHotplug(std::string buffer);

} // namespace drvcore

#endif // POSIX_SUBSYSTEM_DRVCORE_HPP
