#include "src/drvcore.hpp"
#include "tty.hpp"

namespace tty_subsystem {

namespace {

drvcore::ClassSubsystem *sysfsSubsystem;

struct Device final : drvcore::ClassDevice {
	Device(drvcore::ClassSubsystem *subsystem, std::string name, std::shared_ptr<drvcore::Device> parent)
		: drvcore::ClassDevice{subsystem, parent, name, nullptr} {

	}

	void composeUevent(drvcore::UeventProperties &ue) override {
		ue.set("DEVNAME", "tty0");
		ue.set("MINOR", "0");
		ue.set("MAJOR", "4");
	}

	std::optional<std::string> getClassPath() override {
		return "tty";
	};
};

struct ActiveAttribute : sysfs::Attribute {
	ActiveAttribute()
	: sysfs::Attribute{"active", false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

ActiveAttribute activeAttr{};

async::result<frg::expected<Error, std::string>> ActiveAttribute::show(sysfs::Object *) {
	co_return std::format("tty1\n");
}

std::shared_ptr<Device> tty0;

} // namespace

async::detached run() {
	sysfsSubsystem = new drvcore::ClassSubsystem{"tty"};

	tty0 = std::make_shared<Device>(sysfsSubsystem, "tty0", nullptr);
	drvcore::installDevice(tty0);
	tty0->realizeAttribute(&activeAttr);
	co_return;
}

} // namespace tty_subsystem
