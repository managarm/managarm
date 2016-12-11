
#ifndef POSIX_SUBSYSTEM_DEVICE_HPP
#define POSIX_SUBSYSTEM_DEVICE_HPP

#include "vfs.hpp"

// --------------------------------------------------------
// SharedDevice
// --------------------------------------------------------

struct DeviceOperations;

struct Device {
	Device(DeviceId id, const DeviceOperations *operations)
	: _operations(operations) { }

	DeviceId id() {
		return _id;
	}

	const DeviceOperations *operations() {
		return _operations;
	}

private:
	DeviceId _id;
	const DeviceOperations *_operations;
};

struct DeviceOperations {
	FutureMaybe<std::shared_ptr<File>> (*open)(std::shared_ptr<Device> object) = 0;
};

FutureMaybe<std::shared_ptr<File>> open(std::shared_ptr<Device> object);

// --------------------------------------------------------
// DeviceManager
// --------------------------------------------------------

struct DeviceManager {
	void install(std::shared_ptr<Device> device);

	std::shared_ptr<Device> get(DeviceId id);

private:
	struct Compare {
		bool operator() (std::shared_ptr<Device> a, DeviceId b) {
			return a->id() < b;
		}
		bool operator() (DeviceId a, std::shared_ptr<Device> b) {
			return a < b->id();
		}

		bool operator() (std::shared_ptr<Device> a, std::shared_ptr<Device> b) {
			return a->id() < b->id();
		}
	};

	std::set<std::shared_ptr<Device>, Compare> _devices;
};

extern DeviceManager deviceManager;

#endif // POSIX_SUBSYSTEM_DEVICE_HPP

