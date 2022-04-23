#pragma once

#include "vfs.hpp"

// --------------------------------------------------------
// SharedDevice
// --------------------------------------------------------

struct DeviceOperations;

struct UnixDevice {
	UnixDevice(VfsType type)
	: _type{type}, _id{0, 0} { }

protected:
	~UnixDevice() = default;

public:
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

	virtual async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
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

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
openDevice(VfsType type, DeviceId id, std::shared_ptr<MountView> mont, std::shared_ptr<FsLink> link,
		SemanticFlags semantic_flags);

// --------------------------------------------------------
// devtmpfs functions.
// --------------------------------------------------------

std::shared_ptr<FsLink> getDevtmpfs();

async::result<void> createDeviceNode(std::string path, VfsType type, DeviceId id);

// --------------------------------------------------------
// External device helpers.
// --------------------------------------------------------

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
openExternalDevice(helix::BorrowedLane lane,
		std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		SemanticFlags semantic_flags);

FutureMaybe<std::shared_ptr<FsLink>> mountExternalDevice(helix::BorrowedLane lane);
