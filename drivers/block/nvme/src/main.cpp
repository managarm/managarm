#include <format>
#include <iostream>

#include <core/cmdline.hpp>
#include <frg/cmdline.hpp>
#include <map>
#include <protocols/mbus/client.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/svrctl/server.hpp>
#include <netinet/in.h>

#include "controller.hpp"
#include "fabric/tcp.hpp"

std::map<mbus_ng::EntityId, std::unique_ptr<Controller>> globalControllers;

namespace {

async::detached runFabrics(mbus_ng::EntityId entity_id) {
	Cmdline cmdHelper;
	bool use_fabric = false;
	auto cmdline = co_await cmdHelper.get();
	frg::string_view server = "";

	frg::array args = {
		frg::option{"nvme.over-fabric", frg::store_true(use_fabric)},
		frg::option{"netserver.server", frg::as_string_view(server)},
	};
	frg::parse_arguments(cmdline.c_str(), args);

	if(!use_fabric)
		co_return;

	if(globalControllers.contains(entity_id))
		co_return;

	std::string remote = std::string{server.data(), server.size()};
	std::cout << std::format("block/nvme: using NVMe-over-fabric to {}\n", remote);

	auto entity = co_await mbus_ng::Instance::global().getEntity(entity_id);
	auto netserverLane = (co_await entity.getRemoteLane()).unwrap();

	auto convert_ip = [](frg::string_view &str, in_addr *addr) -> bool {
		std::string strbuf{str.data(), str.size()};
		return inet_pton(AF_INET, strbuf.c_str(), addr) == 1;
	};

	in_addr server_ip{};
	convert_ip(server, &server_ip);

	auto controller = std::make_unique<nvme::fabric::Tcp>(-1, server_ip, 4420, std::move(netserverLane));
	controller->run();

	globalControllers.insert({entity_id, std::move(controller)});
}

async::result<protocols::svrctl::Error> bindDevice(int64_t base_id) {
	if(globalControllers.contains(base_id))
		co_return protocols::svrctl::Error::success;

	auto entity = co_await mbus_ng::Instance::global().getEntity(base_id);

	auto properties = (co_await entity.getProperties()).unwrap();
	auto classval = std::get_if<mbus_ng::StringItem>(&properties["class"]);
	auto subsystem = std::get_if<mbus_ng::StringItem>(&properties["unix.subsystem"]);

	if(classval && classval->value == "netserver") {
		runFabrics(base_id);
	} else if(subsystem && subsystem->value == "pci") {
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
