#pragma once

#include <async/recurring-event.hpp>
#include <string>
#include <unordered_map>
#include <protocols/mbus/client.hpp>

#include "device.hpp"
#include "sysfs.hpp"

namespace drvcore {

struct UeventProperties {
	auto begin() {
		return map.begin();
	}

	auto end() {
		return map.end();
	}

	void set(std::string name, std::string value) {
		map[name] = value;
	}

private:
	std::unordered_map<std::string, std::string> map;
};

struct ClassDevice;

struct Subsystem {
	Subsystem(std::shared_ptr<sysfs::Object> obj) : _object(std::move(obj)) {}

	std::shared_ptr<sysfs::Object> object() {
		return _object;
	}

private:
	std::shared_ptr<sysfs::Object> _object;
};

// This struct corresponds to Linux' struct Device (i.e. a device that is part of sysfs).
// TODO: Make the sysfs::Object private?
struct Device : sysfs::Object {
	Device(std::shared_ptr<sysfs::Object> parent, std::string name, UnixDevice *unix_device, Subsystem *subsys = nullptr);

protected:
	~Device() = default;

public:
	void setupDevicePtr(std::weak_ptr<Device> self) {
		_devicePtr = std::move(self);
	}

	std::shared_ptr<Device> devicePtr() {
		return std::shared_ptr<Device>(_devicePtr);
	}

	std::shared_ptr<sysfs::Object> parentDevice() {
		return _parentDevice;
	}

	Subsystem *subsystem() {
		return _subsystem;
	}

	UnixDevice *unixDevice() {
		return _unixDevice;
	}

	std::unordered_map<std::string, std::shared_ptr<sysfs::Object>> &classDirectories() {
		return classDirectories_;
	}

	std::unordered_map<std::string, std::shared_ptr<ClassDevice>> &classDevices() {
		return _classDevices;
	}

	// Returns the path of this device under /sys/devices.
	std::string getSysfsPath();

	void composeStandardUevent(UeventProperties &);

	virtual void linkToSubsystem();

	virtual void composeUevent(UeventProperties &) = 0;

private:
	std::weak_ptr<Device> _devicePtr;
	UnixDevice *_unixDevice;
	std::shared_ptr<sysfs::Object> _parentDevice;
	Subsystem *_subsystem;

	std::unordered_map<std::string, std::shared_ptr<sysfs::Object>> classDirectories_;
	std::unordered_map<std::string, std::shared_ptr<ClassDevice>> _classDevices;
};

struct BusSubsystem : Subsystem {
	BusSubsystem(std::string name);

	std::shared_ptr<sysfs::Object> devicesObject() { return _devicesObject; }
	std::shared_ptr<sysfs::Object> driversObject() { return _driversObject; }

private:
	std::shared_ptr<sysfs::Object> _devicesObject;
	std::shared_ptr<sysfs::Object> _driversObject;
};

struct BusDevice : Device {
	BusDevice(BusSubsystem *subsystem, std::string name,
			UnixDevice *unix_device, std::shared_ptr<Device> parent = nullptr);

protected:
	~BusDevice() = default;

public:
	void linkToSubsystem() override;
};

struct BusDriver : sysfs::Object {
	BusDriver(BusSubsystem *parent, std::string name)
	: sysfs::Object(parent->driversObject(), name) {}
};

struct ClassSubsystem : Subsystem {
	ClassSubsystem(std::string name);

	friend ClassDevice;
private:
	std::shared_ptr<sysfs::Object> classDirFor(std::shared_ptr<Device> parent) {
		if(!parent)
			return parent;

		auto it = parent->classDirectories().find(object()->name());
		if(it != parent->classDirectories().end())
			return it->second;

		auto classDir = std::make_shared<sysfs::Object>(parent, object()->name());
		classDir->addObject();
		parent->classDirectories().insert({object()->name(), classDir});
		return classDir;
	}
};

struct ClassDevice : Device {
	ClassDevice(ClassSubsystem *subsystem, std::shared_ptr<Device> parent,
			std::string name, UnixDevice *unix_device);

protected:
	~ClassDevice() = default;

public:
	void linkToSubsystem() override;

private:
	std::shared_ptr<Device> parentDevice_;
};

struct BlockDevice : Device {
	BlockDevice(ClassSubsystem *subsystem, std::shared_ptr<Device> parent,
			std::string name, UnixDevice *unix_device);

protected:
	~BlockDevice() = default;

public:
	void linkToSubsystem() override;
};

void initialize();

sysfs::Object *virtualDeviceParent();

void registerMbusDevice(mbus_ng::EntityId, std::shared_ptr<Device>);
extern async::recurring_event mbusMapUpdate;
std::shared_ptr<Device> getMbusDevice(mbus_ng::EntityId);

void installDevice(std::shared_ptr<Device> device);

namespace udev {

void emitAddEvent(std::string devpath, UeventProperties &ue);
void emitChangeEvent(std::string devpath, UeventProperties &ue);

} // namespace udev

} // namespace drvcore
