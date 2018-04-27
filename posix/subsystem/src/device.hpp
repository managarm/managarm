#ifndef POSIX_SUBSYSTEM_DEVICE_HPP
#define POSIX_SUBSYSTEM_DEVICE_HPP

#include "vfs.hpp"

// --------------------------------------------------------
// SharedDevice
// --------------------------------------------------------

struct DeviceOperations;

struct UnixDevice {
	UnixDevice(VfsType type)
	: _type{type}, _id{0, 0} { }

	VfsType type() {
		return _type;
	}

	void assignId(DeviceId id) {
		_id = id;
	}

	DeviceId getId() {
		return _id;
	}

	virtual std::string nodePath() = 0;

	virtual FutureMaybe<smarter::shared_ptr<File, FileHandle>> open(std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) = 0;

	virtual FutureMaybe<std::shared_ptr<FsLink>> mount();

private:
	VfsType _type;
	DeviceId _id;
};

// --------------------------------------------------------
// UnixDeviceRegistry
// --------------------------------------------------------

struct UnixDeviceRegistry {
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

extern UnixDeviceRegistry charRegistry;
extern UnixDeviceRegistry blockRegistry;

FutureMaybe<smarter::shared_ptr<File, FileHandle>> openDevice(VfsType type, DeviceId id,
		std::shared_ptr<FsLink> link, SemanticFlags semantic_flags);

// --------------------------------------------------------
// devtmpfs functions.
// --------------------------------------------------------

std::shared_ptr<FsLink> getDevtmpfs();

async::result<void> createDeviceNode(std::string path, VfsType type, DeviceId id);

// --------------------------------------------------------
// External device helpers.
// --------------------------------------------------------

FutureMaybe<smarter::shared_ptr<File, FileHandle>> openExternalDevice(helix::BorrowedLane lane,
		std::shared_ptr<FsLink> link, SemanticFlags semantic_flags);

FutureMaybe<std::shared_ptr<FsLink>> mountExternalDevice(helix::BorrowedLane lane);

#endif // POSIX_SUBSYSTEM_DEVICE_HPP
