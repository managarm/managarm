#ifndef POSIX_SUBSYSTEM_DEVICE_HPP
#define POSIX_SUBSYSTEM_DEVICE_HPP

#include "vfs.hpp"

// --------------------------------------------------------
// SharedDevice
// --------------------------------------------------------

struct DeviceOperations;

struct UnixDevice {
	static VfsType getType(std::shared_ptr<UnixDevice> object);
	static std::string getName(std::shared_ptr<UnixDevice> object);
	static FutureMaybe<std::shared_ptr<FsLink>> mount(std::shared_ptr<UnixDevice> object);

	UnixDevice(const DeviceOperations *operations)
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
	VfsType (*getType)(std::shared_ptr<UnixDevice> object) = 0;
	std::string (*getName)(std::shared_ptr<UnixDevice> object) = 0;

	FutureMaybe<std::shared_ptr<File>> (*open)(std::shared_ptr<UnixDevice> object,
			std::shared_ptr<FsLink> link) = 0;
	FutureMaybe<std::shared_ptr<FsLink>> (*mount)(std::shared_ptr<UnixDevice> object) = 0;
};

FutureMaybe<std::shared_ptr<File>> open(std::shared_ptr<UnixDevice> object,
		std::shared_ptr<FsLink> link);

// --------------------------------------------------------
// UnixDeviceManager
// --------------------------------------------------------

struct UnixDeviceManager {
	void install(std::shared_ptr<UnixDevice> device);

	std::shared_ptr<UnixDevice> get(DeviceId id);

private:
	struct Compare {
		struct is_transparent { };

		bool operator() (std::shared_ptr<UnixDevice> a, DeviceId b) const {
			return a->getId() < b;
		}
		bool operator() (DeviceId a, std::shared_ptr<UnixDevice> b) const {
			return a < b->getId();
		}

		bool operator() (std::shared_ptr<UnixDevice> a, std::shared_ptr<UnixDevice> b) const {
			return a->getId() < b->getId();
		}
	};

	std::set<std::shared_ptr<UnixDevice>, Compare> _devices;
};

std::shared_ptr<FsLink> getDevtmpfs();

extern UnixDeviceManager deviceManager;

#endif // POSIX_SUBSYSTEM_DEVICE_HPP
