
#include <string.h>

#include "common.hpp"
#include "device.hpp"
#include "tmp_fs.hpp"

UnixDeviceManager deviceManager;

// --------------------------------------------------------
// UnixDevice
// --------------------------------------------------------

FutureMaybe<std::shared_ptr<FsLink>> UnixDevice::mount() {
	// TODO: Return an error.
	throw std::logic_error("Device cannot be mounted!");
}

// --------------------------------------------------------
// UnixDeviceManager
// --------------------------------------------------------

void UnixDeviceManager::install(std::shared_ptr<UnixDevice> device) {
	DeviceId id{0, 1};
	while(_devices.find(id) != _devices.end())
		id.second++;
	device->assignId(id);
	// TODO: Ensure that the insert succeeded.
	_devices.insert(device);

	getDevtmpfs()->getTarget()->mkdev(device->getName(),
			device->type(), id);
}

std::shared_ptr<UnixDevice> UnixDeviceManager::get(DeviceId id) {
	auto it = _devices.find(id);
	assert(it != _devices.end());
	return *it;
}

// --------------------------------------------------------
// Free functions.
// --------------------------------------------------------

std::shared_ptr<FsLink> getDevtmpfs() {
	static std::shared_ptr<FsLink> devtmpfs = tmp_fs::createRoot();
	return devtmpfs;
}

