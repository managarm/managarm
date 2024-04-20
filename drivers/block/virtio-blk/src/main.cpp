
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <memory>
#include <queue>
#include <vector>

#include <async/result.hpp>
#include <hel.h>
#include <hel-syscalls.h>
#include <helix/ipc.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/hw/client.hpp>

#include "block.hpp"

// TODO: Support more than one device.

async::detached bindDevice(mbus_ng::Entity hwEntity) {
	protocols::hw::Device hwDevice((co_await hwEntity.getRemoteLane()).unwrap());
	auto transport = co_await virtio_core::discover(std::move(hwDevice),
			virtio_core::DiscoverMode::transitional);

	auto device = new block::virtio::Device{std::move(transport), hwEntity.id()};
	device->runDevice();

/*
	auto info = co_await hw_device.getPciInfo();
	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypePort);
	auto bar = co_await hw_device.accessBar(0);
	auto irq = co_await hw_device.accessIrq();

	HEL_CHECK(helEnableIo(bar.getHandle()));

	std::cout << "Setting up the device" << std::endl;
	device.setupDevice(info.barInfo[0].address, std::move(irq));
*/
}

async::detached observeDevices() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"pci-vendor", "1af4"},
		mbus_ng::EqualsFilter{"pci-device", "1001"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			std::cout << "virtio-blk: Detected controller" << std::endl;
			bindDevice(std::move(entity));
		}
	}
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting virtio-block driver\n");

//	HEL_CHECK(helSetPriority(kHelThisThread, 3));

	observeDevices();
	async::run_forever(helix::currentDispatcher);
}

