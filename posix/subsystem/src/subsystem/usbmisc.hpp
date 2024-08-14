#pragma once

#include "src/drvcore.hpp"
#include "src/subsystem/usb/drivers.hpp"

namespace usbmisc_subsystem {

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

} // namespace usbmisc_subsystem
