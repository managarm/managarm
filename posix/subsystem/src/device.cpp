
#include <string.h>

#include "common.hpp"
#include "device.hpp"
#include "tmp_fs.hpp"

UnixDeviceManager deviceManager;

// --------------------------------------------------------
// UnixDevice
// --------------------------------------------------------

VfsType UnixDevice::getType(std::shared_ptr<UnixDevice> object) {
	return object->operations()->getType(object);
}
std::string UnixDevice::getName(std::shared_ptr<UnixDevice> object) {
	return object->operations()->getName(object);
}

FutureMaybe<std::shared_ptr<File>> open(std::shared_ptr<UnixDevice> device,
		std::shared_ptr<FsLink> link) {
	return device->operations()->open(device, std::move(link));
}
FutureMaybe<std::shared_ptr<FsLink>> UnixDevice::mount(std::shared_ptr<UnixDevice> object) {
	return object->operations()->mount(object);
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

	getDevtmpfs()->getTarget()->mkdev(UnixDevice::getName(device),
			UnixDevice::getType(device), id);
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

