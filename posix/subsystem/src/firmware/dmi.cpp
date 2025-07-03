#include "dmi.hpp"

#include "../drvcore.hpp"
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>

namespace {

struct Table;

struct SmbiosEntryPointAttribute final : sysfs::Attribute {
	SmbiosEntryPointAttribute(std::vector<uint8_t> smbiosHeader)
	: sysfs::Attribute{"smbios_entry_point", false},
	  smbiosHeader_{std::move(smbiosHeader)} {
		_size = smbiosHeader_.size();
	}

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;

private:
	std::vector<uint8_t> smbiosHeader_;
};

struct DmiAttribute final : sysfs::Attribute {
	DmiAttribute(std::vector<uint8_t> smbiosTable)
	: sysfs::Attribute{"DMI", false},
	  smbiosTable_{std::move(smbiosTable)} {
		_size = smbiosTable_.size();
	}

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;

private:
	std::vector<uint8_t> smbiosTable_;
};

struct Table final : sysfs::Object {
	Table(std::shared_ptr<sysfs::Object> parent, protocols::hw::Device device)
	: sysfs::Object{parent, "tables"},
	  device_{std::move(device)} {}

	protocols::hw::Device &getDevice() { return device_; }

	async::result<void> publish() {
		smbiosEntryPointAttribute =
		    std::make_unique<SmbiosEntryPointAttribute>(co_await device_.getSmbiosHeader());
		dmiAttribute = std::make_unique<DmiAttribute>(co_await device_.getSmbiosTable());

		realizeAttribute(smbiosEntryPointAttribute.get());
		realizeAttribute(static_cast<sysfs::Attribute *>(dmiAttribute.get()));
	}

private:
	protocols::hw::Device device_;
	std::unique_ptr<SmbiosEntryPointAttribute> smbiosEntryPointAttribute;
	std::unique_ptr<DmiAttribute> dmiAttribute;
};

async::result<frg::expected<Error, std::string>> SmbiosEntryPointAttribute::show(sysfs::Object *) {
	co_return std::string(smbiosHeader_.begin(), smbiosHeader_.end());
}

async::result<frg::expected<Error, std::string>> DmiAttribute::show(sysfs::Object *) {
	co_return std::string(smbiosTable_.begin(), smbiosTable_.end());
}

std::shared_ptr<sysfs::Object> dmiObject;
std::shared_ptr<Table> tablesObject;

async::detached bind(mbus_ng::Entity entity) {
	protocols::hw::Device device((co_await entity.getRemoteLane()).unwrap());
	dmiObject = std::make_shared<sysfs::Object>(drvcore::firmwareObject(), "dmi");
	dmiObject->addObject();

	tablesObject = std::make_shared<Table>(dmiObject, std::move(device));
	tablesObject->addObject();
	co_await tablesObject->publish();
}

} // namespace

namespace firmware_dmi {

async::detached run() {
	auto filter = mbus_ng::Conjunction({
	    mbus_ng::EqualsFilter{"unix.subsystem", "firmware"},
	    mbus_ng::EqualsFilter{"firmware.type", "smbios"},
	});

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);

	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			if (!event.properties.contains("version")
			    || std::get_if<mbus_ng::StringItem>(&event.properties.at("version"))->value
			           != "3") {
				co_return;
			}

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			bind(std::move(entity));

			co_return;
		}
	}
}

} // namespace firmware_dmi
