#include <iostream>

#include <protocols/mbus/client.hpp>
#include <protocols/hw/client.hpp>

#include "controller.hpp"

std::vector<std::unique_ptr<Controller>> globalControllers;

async::detached bindController(mbus_ng::Entity entity) {
	protocols::hw::Device device((co_await entity.getRemoteLane()).unwrap());
	auto info = co_await device.getPciInfo();

	auto &barInfo = info.barInfo[0];
	assert(barInfo.ioType == protocols::hw::IoType::kIoTypeMemory);
	auto bar0 = co_await device.accessBar(0);
	auto irq = co_await device.accessIrq();

	helix::Mapping mapping{bar0, barInfo.offset, barInfo.length};

	auto controller = std::make_unique<Controller>(entity.id(), std::move(device), std::move(mapping),
			std::move(bar0), std::move(irq));
	controller->run();
	globalControllers.push_back(std::move(controller));
}

async::detached observeControllers() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"pci-class", "01"},
		mbus_ng::EqualsFilter{"pci-subclass", "08"},
		mbus_ng::EqualsFilter{"pci-interface", "02"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			std::cout << "block/nvme: Detected controller" << std::endl;
			bindController(std::move(entity));
		}
	}
}

int main() {
	std::cout << "block/nvme: Starting driver\n";

	observeControllers();
	async::run_forever(helix::currentDispatcher);
}
