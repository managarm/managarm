
#include <linux/netlink.h>

#include "drvcore.hpp"
#include "nl-socket.hpp"

namespace drvcore {

std::shared_ptr<sysfs::Object> globalDevicesObject;
std::shared_ptr<sysfs::Object> globalClassObject;
std::shared_ptr<sysfs::Object> globalCharObject;
std::shared_ptr<sysfs::Object> globalBlockObject;

std::shared_ptr<sysfs::Object> inputClassObject;

std::shared_ptr<sysfs::Object> cardObject;
std::shared_ptr<sysfs::Object> eventObject;

sysfs::Object *devicesObject() {
	assert(globalDevicesObject);
	return globalDevicesObject.get();
}

sysfs::Object *classObject() {
	assert(globalClassObject);
	return globalClassObject.get();
}

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
		std::cout << "\e[31mposix: uevent files are static\e[39m" << std::endl;
		if(object == cardObject.get()) {
			COFIBER_RETURN(std::string{"DEVNAME=dri/card0\n"});
		}else if(eventObject && object == eventObject.get()) {
			COFIBER_RETURN(std::string{"DEVNAME=input/event0\n"
				"MAJOR=13\n"
				"MINOR=64\n"});
		}else{
			throw std::runtime_error("posix: Unexpected object for UeventAttribute::show()");
		}
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
	cardObject->realizeAttribute(UeventAttribute::singleton());

	auto drm_object = std::make_shared<sysfs::Object>(globalClassObject, "drm");
	drm_object->addObject();
	drm_object->createSymlink("card0", cardObject);

	inputClassObject = std::make_shared<sysfs::Object>(globalClassObject, "input");
	inputClassObject->addObject();
}

void installDevice(std::shared_ptr<Device> device) {
	eventObject = device;

	eventObject->addObject();
	
	// TODO: Do this before the object becomes visible in sysfs.
	eventObject->realizeAttribute(UeventAttribute::singleton());
	
	inputClassObject->createSymlink(device->name(), eventObject);
	globalCharObject->createSymlink("13:64", eventObject);
}

void emitHotplug(std::string buffer) {
	nl_socket::broadcast(NETLINK_KOBJECT_UEVENT, 1, std::move(buffer));
}

} // namespace drvcore

