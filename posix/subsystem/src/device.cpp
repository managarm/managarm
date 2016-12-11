
#include <string.h>

#include "common.hpp"
#include "device.hpp"

DeviceManager deviceManager;

// --------------------------------------------------------
// Device
// --------------------------------------------------------

FutureMaybe<std::shared_ptr<File>> open(std::shared_ptr<Device> device) {
	return device->operations()->open(device);
}

// --------------------------------------------------------
// DeviceManager
// --------------------------------------------------------

std::shared_ptr<Device> DeviceManager::get(DeviceId id) {
	assert(!"Implement this");
}

