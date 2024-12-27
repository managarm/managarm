#include <iostream>

#include <map>
#include <protocols/mbus/client.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/svrctl/server.hpp>

#include "controller.hpp"

std::map<mbus_ng::EntityId, std::unique_ptr<Controller>> globalControllers;

namespace {

async::result<protocols::svrctl::Error> bindDevice(int64_t base_id) {
	if(globalControllers.contains(base_id))
		co_return protocols::svrctl::Error::success;

	auto entity = co_await mbus_ng::Instance::global().getEntity(base_id);

	auto properties = (co_await entity.getProperties()).unwrap();
	auto subsystem = std::get_if<mbus_ng::StringItem>(&properties["unix.subsystem"]);

	if(subsystem && subsystem->value == "pci") {
		if(std::get<mbus_ng::StringItem>(properties.at("pci-class")).value != "01"
		|| std::get<mbus_ng::StringItem>(properties.at("pci-subclass")).value != "08"
		|| std::get<mbus_ng::StringItem>(properties.at("pci-interface")).value != "02")
			co_return protocols::svrctl::Error::deviceNotSupported;

		protocols::hw::Device device((co_await entity.getRemoteLane()).unwrap());
		auto info = co_await device.getPciInfo();

		auto &barInfo = info.barInfo[0];
		assert(barInfo.ioType == protocols::hw::IoType::kIoTypeMemory);
		auto bar0 = co_await device.accessBar(0);
		auto irq = co_await device.accessIrq();

		helix::Mapping mapping{bar0, barInfo.offset, barInfo.length};

		auto controller = std::make_unique<PciExpressController>(base_id, std::move(device), std::move(mapping),
				std::move(irq));
		controller->run();
		globalControllers.insert({base_id, std::move(controller)});
	} else {
		co_return protocols::svrctl::Error::deviceNotSupported;
	}

	co_return protocols::svrctl::Error::success;
}

constexpr protocols::svrctl::ControlOperations controlOps = {
	.bind = bindDevice,
};

} // namespace

int main() {
	std::cout << "block/nvme: Starting driver\n";

	async::detach(protocols::svrctl::serveControl(&controlOps));
	async::run_forever(helix::currentDispatcher);
}
