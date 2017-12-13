#ifndef POSIX_SUBSYSTEM_DEVICE_HPP
#define POSIX_SUBSYSTEM_DEVICE_HPP

#include "vfs.hpp"

// --------------------------------------------------------
// SharedDevice
// --------------------------------------------------------

struct DeviceOperations;

struct Device {
	static VfsType getType(std::shared_ptr<Device> object);
	static std::string getName(std::shared_ptr<Device> object);
	static FutureMaybe<std::shared_ptr<Link>> mount(std::shared_ptr<Device> object);

	Device(const DeviceOperations *operations)
	: _operations(operations) { }

	void assignId(DeviceId id) {
		_id = id;
	}

	DeviceId getId() {
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
	VfsType (*getType)(std::shared_ptr<Device> object) = 0;
	std::string (*getName)(std::shared_ptr<Device> object) = 0;

	FutureMaybe<std::shared_ptr<File>> (*open)(std::shared_ptr<Device> object,
			std::shared_ptr<Link> link) = 0;
	FutureMaybe<std::shared_ptr<Link>> (*mount)(std::shared_ptr<Device> object) = 0;
};

FutureMaybe<std::shared_ptr<File>> open(std::shared_ptr<Device> object,
		std::shared_ptr<Link> link);

// --------------------------------------------------------
// DeviceManager
// --------------------------------------------------------

struct DeviceManager {
	void install(std::shared_ptr<Device> device);

	std::shared_ptr<Device> get(DeviceId id);

private:
	struct Compare {
		struct is_transparent { };

		bool operator() (std::shared_ptr<Device> a, DeviceId b) const {
			return a->getId() < b;
		}
		bool operator() (DeviceId a, std::shared_ptr<Device> b) const {
			return a < b->getId();
		}

		bool operator() (std::shared_ptr<Device> a, std::shared_ptr<Device> b) const {
			return a->getId() < b->getId();
		}
	};

	std::set<std::shared_ptr<Device>, Compare> _devices;
};

std::shared_ptr<Link> getDevtmpfs();

extern DeviceManager deviceManager;

#endif // POSIX_SUBSYSTEM_DEVICE_HPP
