#include "src/drvcore.hpp"
#include "src/subsystem/generic.hpp"
#include "src/subsystem/usb/drivers.hpp"
#include "src/subsystem/usb/usb.hpp"
#include "usbmisc.hpp"

namespace {

drvcore::ClassSubsystem *sysfsSubsystem;

struct Device final : drvcore::ClassDevice {
	Device(drvcore::ClassSubsystem *subsystem, std::string name,
		std::shared_ptr<drvcore::Device> parent)
		: drvcore::ClassDevice{subsystem, parent, std::move(name), nullptr} {
	}

	void composeUevent(drvcore::UeventProperties &ue) override {
		ue.set("DEVNAME", "/dev/cdc-wdm0");
		ue.set("SUBSYSTEM", "usbmisc");
	}

	std::optional<std::string> getClassPath() override {
		return "usbmisc";
	};

private:
	std::shared_ptr<CdcMbimDriver> _mbimDriver;
};

std::unordered_map<mbus_ng::EntityId, std::shared_ptr<Device>> mbusMap;

} // namespace

namespace usbmisc_subsystem {

async::detached run() {
	sysfsSubsystem = new drvcore::ClassSubsystem{"usbmisc"};

	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"unix.subsystem", "usbmisc"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			auto parent_id = std::get<mbus_ng::StringItem>(event.properties.at("drvcore.mbus-parent")).value;
			auto parent_dev = drvcore::getMbusDevice(std::stoi(parent_id));

			if(event.properties.contains("usb.parent-interface"))
				parent_dev = co_await usb_subsystem::getInterfaceDevice(parent_dev, event.properties);

			auto devinfo = generic_subsystem::getDeviceName(std::stoi(parent_id));
			if(!devinfo)
				continue;
			auto devname = std::format("{}{}", devinfo->first, devinfo->second);
			auto usbmisc = std::make_shared<Device>(sysfsSubsystem, devname, parent_dev);
			drvcore::installDevice(usbmisc);
			mbusMap.insert({entity.id(), std::move(usbmisc)});
		}
	}
}

} // namespace usbmisc_subsystem
