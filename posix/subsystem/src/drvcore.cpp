
#include <linux/netlink.h>
#include <sstream>

#include "drvcore.hpp"
#include "nl-socket.hpp"

namespace drvcore {

std::shared_ptr<sysfs::Object> globalDevicesObject;
std::shared_ptr<sysfs::Object> globalClassObject;
std::shared_ptr<sysfs::Object> globalCharObject;
std::shared_ptr<sysfs::Object> globalBlockObject;

std::shared_ptr<sysfs::Object> inputClassObject;

std::shared_ptr<sysfs::Object> cardObject;

sysfs::Object *devicesObject() {
	assert(globalDevicesObject);
	return globalDevicesObject.get();
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
	: sysfs::Attribute("uevent") { }

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
	: sysfs::Attribute("uevent") { }

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
	globalClassObject = std::make_shared<sysfs::Object>(nullptr, "class");
	globalDevicesObject->addObject();
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

	inputClassObject = std::make_shared<sysfs::Object>(globalClassObject, "input");
	inputClassObject->addObject();
}

void installDevice(std::shared_ptr<Device> device) {
	device->addObject();
	
	// TODO: Do this before the object becomes visible in sysfs.
	device->realizeAttribute(UeventAttribute::singleton());
	device->createSymlink("subsystem", inputClassObject);
	
	inputClassObject->createSymlink(device->name(), device);
	
	if(auto unix_dev = device->unixDevice(); unix_dev) {
		std::stringstream id_ss;
		id_ss << unix_dev->getId().first << ":" << unix_dev->getId().second;
		assert(unix_dev->type() == VfsType::charDevice);
		globalCharObject->createSymlink(id_ss.str(), device);
	}
}

void emitHotplug(std::string buffer) {
	nl_socket::broadcast(NETLINK_KOBJECT_UEVENT, 1, std::move(buffer));
}

} // namespace drvcore

