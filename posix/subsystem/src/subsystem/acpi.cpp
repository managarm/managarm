#include <async/recurring-event.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/hw/client.hpp>

#include "src/drvcore.hpp"
#include "acpi.hpp"

namespace acpi_subsystem {

drvcore::BusSubsystem *sysfsSubsystem;

std::unordered_map<int, std::shared_ptr<drvcore::Device>> mbusMap;

struct HidAttribute : sysfs::Attribute {
	HidAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct PathAttribute : sysfs::Attribute {
	PathAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct UidAttribute : sysfs::Attribute {
	UidAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

HidAttribute hidAttr{"hid"};
PathAttribute pathAttr{"path"};
UidAttribute uidAttr{"uid"};

struct Device final : drvcore::BusDevice {
	Device(std::string sysfs_name, int64_t mbus_id, std::string path, std::string hid_name,
		unsigned int id, std::shared_ptr<drvcore::Device> parent = nullptr)
	: drvcore::BusDevice{sysfsSubsystem, std::move(sysfs_name), nullptr, parent},
			mbusId{mbus_id}, path{path}, hid{hid_name}, instance{id} { }

	void composeUevent(drvcore::UeventProperties &ue) override {
		ue.set("SUBSYSTEM", "acpi");
		ue.set("MBUS_ID", std::to_string(mbusId));
	}

	int64_t mbusId;
	std::string path;
	std::string hid;
	std::string uid;
	unsigned int instance;
};

async::result<frg::expected<Error, std::string>> HidAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	co_return std::format("{}\n", device->hid);
}

async::result<frg::expected<Error, std::string>> PathAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	co_return std::format("{}\n", device->path);
}

async::result<frg::expected<Error, std::string>> UidAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	co_return std::format("{}\n", device->uid);
}

async::detached bind(mbus_ng::Entity entity, mbus_ng::Properties properties) {
	auto hid_name = std::get<mbus_ng::StringItem>(properties["acpi.hid"]).value;
	auto path = std::get<mbus_ng::StringItem>(properties["acpi.path"]).value;
	auto uid_str = std::get_if<mbus_ng::StringItem>(&properties["acpi.uid"]);
	auto instance = std::stoul(std::get<mbus_ng::StringItem>(properties["acpi.instance"]).value);
	assert(instance < std::numeric_limits<unsigned int>::max());

	auto sysfs_name = std::format("{}:{:02}", hid_name, instance);
	auto device = std::make_shared<Device>(sysfs_name, entity.id(), path, hid_name, instance);
	drvcore::installDevice(device);

	device->realizeAttribute(&hidAttr);
	device->realizeAttribute(&pathAttr);

	if(uid_str) {
		device->uid = uid_str->value;
		device->realizeAttribute(&uidAttr);
	}

	drvcore::registerMbusDevice(entity.id(), device);

	if(properties.contains("acpi.physical_node")) {
		auto physical_node = std::get<mbus_ng::StringItem>(properties["acpi.physical_node"]).value;
		while(true) {
			co_await drvcore::mbusMapUpdate.async_wait();
			auto target = drvcore::getMbusDevice(std::stoul(physical_node));
			if(target) {
				device->createSymlink("physical_node", target);
				break;
			}
		}
	}

	co_return;
}

async::detached run() {
	sysfsSubsystem = new drvcore::BusSubsystem{"acpi"};

	auto filter = mbus_ng::Conjunction({
		mbus_ng::EqualsFilter{"unix.subsystem", "acpi"}
	});

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);

			if(event.properties.contains("acpi.hid"))
				bind(std::move(entity), std::move(event.properties));
		}
	}
}

} // namespace acpi_subsystem

