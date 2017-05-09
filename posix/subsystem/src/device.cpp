
#include <string.h>

#include "common.hpp"
#include "device.hpp"
#include "tmp_fs.hpp"

DeviceManager deviceManager;

// --------------------------------------------------------
// Device
// --------------------------------------------------------

VfsType Device::getType(std::shared_ptr<Device> object) {
	return object->operations()->getType(object);
}
std::string Device::getName(std::shared_ptr<Device> object) {
	return object->operations()->getName(object);
}

FutureMaybe<std::shared_ptr<File>> open(std::shared_ptr<Device> device,
		std::shared_ptr<Node> node) {
	return device->operations()->open(device, std::move(node));
}
FutureMaybe<std::shared_ptr<Link>> Device::mount(std::shared_ptr<Device> object) {
	return object->operations()->mount(object);
}

// --------------------------------------------------------
// DeviceManager
// --------------------------------------------------------

void DeviceManager::install(std::shared_ptr<Device> device) {
	DeviceId id{0, 1};
	while(_devices.find(id) != _devices.end())
		id.second++;
	device->assignId(id);
	// TODO: Ensure that the insert succeeded.
	_devices.insert(device);

	mkdev(getTarget(getDevtmpfs()), Device::getName(device),
			Device::getType(device), id);
}

std::shared_ptr<Device> DeviceManager::get(DeviceId id) {
	auto it = _devices.find(id);
	assert(it != _devices.end());
	return *it;
}

// --------------------------------------------------------
// Free functions.
// --------------------------------------------------------

std::shared_ptr<Link> getDevtmpfs() {
	static std::shared_ptr<Link> devtmpfs = tmp_fs::createRoot();
	return devtmpfs;
}

