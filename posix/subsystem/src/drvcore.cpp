
#include <linux/netlink.h>
#include <sstream>

#include "drvcore.hpp"
#include "nl-socket.hpp"

namespace drvcore {

std::shared_ptr<sysfs::Object> globalDevicesObject;
std::shared_ptr<sysfs::Object> globalBusObject;
std::shared_ptr<sysfs::Object> globalClassObject;
std::shared_ptr<sysfs::Object> globalCharObject;
std::shared_ptr<sysfs::Object> globalBlockObject;

std::shared_ptr<sysfs::Object> cardObject;

sysfs::Object *devicesObject() {
	assert(globalDevicesObject);
	return globalDevicesObject.get();
}

sysfs::Object *busObject() {
	assert(globalBusObject);
	return globalBusObject.get();
}

sysfs::Object *classObject() {
	assert(globalClassObject);
	return globalClassObject.get();
}

struct Card0UeventAttribute : sysfs::Attribute {
	static auto singleton() {
		static Card0UeventAttribute attr;
		return &attr;
	}

private:
	Card0UeventAttribute()
	: sysfs::Attribute("uevent", true) { }

public:
	virtual COFIBER_ROUTINE(async::result<std::string>, show(sysfs::Object *object) override, ([=] {
		assert(object == cardObject.get());
		COFIBER_RETURN(std::string{"DEVNAME=dri/card0\n"});
	}))
};

struct UeventAttribute : sysfs::Attribute {
	static auto singleton() {
		static UeventAttribute attr;
		return &attr;
	}

private:
	UeventAttribute()
	: sysfs::Attribute("uevent", true) { }

public:
	virtual COFIBER_ROUTINE(async::result<std::string>, show(sysfs::Object *object) override, ([=] {
		auto device = static_cast<Device *>(object);

		std::stringstream ss;
		if(auto unix_dev = device->unixDevice(); unix_dev) {
			auto node_path = unix_dev->nodePath();
			if(!node_path.empty())
				ss << "DEVNAME=" << node_path << '\n';
			ss << "MAJOR=" << unix_dev->getId().first << '\n';
			ss << "MINOR=" << unix_dev->getId().second << '\n';
		}
		COFIBER_RETURN(ss.str());
	}))
};

//-----------------------------------------------------------------------------
// Device implementation.
//-----------------------------------------------------------------------------

Device::Device(std::shared_ptr<Device> parent, std::string name, UnixDevice *unix_device)
: sysfs::Object{parent ? parent : globalDevicesObject, std::move(name)},
		_unixDevice{unix_device} { }

void Device::linkToSubsystem() {
	// Nothing to do for devices outside of a subsystem.
}

//-----------------------------------------------------------------------------
// BusSubsystem and BusDevice implementation.
//-----------------------------------------------------------------------------

BusSubsystem::BusSubsystem(std::string name)
: _object{std::make_shared<sysfs::Object>(globalBusObject, std::move(name))} {
	_object->addObject();
	_devicesObject = std::make_shared<sysfs::Object>(_object, "devices");
	_devicesObject->addObject();
	_driversObject = std::make_shared<sysfs::Object>(_object, "drivers");
	_driversObject->addObject();
}

BusDevice::BusDevice(BusSubsystem *subsystem, std::string name,
		UnixDevice *unix_device)
: Device{nullptr, std::move(name), unix_device},
		_subsystem{subsystem} { }

void BusDevice::linkToSubsystem() {
	auto subsystem_object = _subsystem->devicesObject();
	subsystem_object->createSymlink(name(), devicePtr());
	createSymlink("subsystem", subsystem_object);
}

//-----------------------------------------------------------------------------
// ClassSubsystem and ClassDevice implementation.
//-----------------------------------------------------------------------------

ClassSubsystem::ClassSubsystem(std::string name)
: _object{std::make_shared<sysfs::Object>(globalClassObject, std::move(name))} {
	_object->addObject();
}

ClassDevice::ClassDevice(ClassSubsystem *subsystem, std::string name,
		UnixDevice *unix_device)
: Device{nullptr, std::move(name), unix_device},
		_subsystem{subsystem} { }

void ClassDevice::linkToSubsystem() {
	auto subsystem_object = _subsystem->object();
	subsystem_object->createSymlink(name(), devicePtr());
	createSymlink("subsystem", subsystem_object);
}

//-----------------------------------------------------------------------------
// Free functions.
//-----------------------------------------------------------------------------

void initialize() {
	nl_socket::configure(NETLINK_KOBJECT_UEVENT, 32);

	// Create the /sys/dev/{char,block} directories.
	auto dev_object = std::make_shared<sysfs::Object>(nullptr, "dev");
	globalCharObject = std::make_shared<sysfs::Object>(dev_object, "char");
	globalBlockObject = std::make_shared<sysfs::Object>(dev_object, "block");

	// Create the global /sys/{devices,class,dev} directories.
	globalDevicesObject = std::make_shared<sysfs::Object>(nullptr, "devices");
	globalBusObject = std::make_shared<sysfs::Object>(nullptr, "bus");
	globalClassObject = std::make_shared<sysfs::Object>(nullptr, "class");
	globalDevicesObject->addObject();
	globalBusObject->addObject();
	globalClassObject->addObject();
	dev_object->addObject();
	globalCharObject->addObject(); // TODO: Do this before dev_object is visible.
	globalBlockObject->addObject();

	cardObject = std::make_shared<sysfs::Object>(globalDevicesObject, "card0");
	cardObject->addObject();
	cardObject->realizeAttribute(Card0UeventAttribute::singleton());

	auto drm_object = std::make_shared<sysfs::Object>(globalClassObject, "drm");
	drm_object->addObject();
	drm_object->createSymlink("card0", cardObject);
}

void installDevice(std::shared_ptr<Device> device) {
	device->setupDevicePtr(device);
	device->addObject();

	std::string sysfs_path = device->name();
	auto parent = device->directoryNode()->treeLink()->getOwner();
	auto link = parent->treeLink().get();
	while(true) {
		auto owner = link->getOwner();
		if(!owner)
			break;
		sysfs_path = link->getName() + '/' + sysfs_path;
		link = owner->treeLink().get();
	}

	// TODO: Do this before the object becomes visible in sysfs.
	device->linkToSubsystem();
	device->realizeAttribute(UeventAttribute::singleton());

	if(auto unix_dev = device->unixDevice(); unix_dev) {
		std::stringstream id_ss;
		id_ss << unix_dev->getId().first << ":" << unix_dev->getId().second;
		assert(unix_dev->type() == VfsType::charDevice);
		globalCharObject->createSymlink(id_ss.str(), device);
	}

	std::stringstream ss;
	ss << "add@/" << sysfs_path << '\0';
	ss << "ACTION=add" << '\0';
	ss << "DEVPATH=" << sysfs_path << '\0';
	device->composeUevent(ss);
	ss << "SEQNUM=" << drvcore::makeHotplugSeqnum() << '\0';
	drvcore::emitHotplug(ss.str());
}

// TODO: There could be a race between makeHotplugSeqnum() and emitHotplug().
//       Is it required that seqnums always appear in the correct order?
uint32_t makeHotplugSeqnum() {
	static uint32_t seqnum = 1;
	return seqnum++;
}

void emitHotplug(std::string buffer) {
	nl_socket::broadcast(NETLINK_KOBJECT_UEVENT, 1, std::move(buffer));
}

} // namespace drvcore

