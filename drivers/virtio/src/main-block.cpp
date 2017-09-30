
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <memory>
#include <queue>
#include <vector>

#include <cofiber.hpp>
#include <hel.h>
#include <hel-syscalls.h>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/hw/client.hpp>

#include "block.hpp"

// TODO: Support more than one device.

COFIBER_ROUTINE(cofiber::no_future, bindDevice(mbus::Entity entity), ([=] {
	protocols::hw::Device hw_device(COFIBER_AWAIT entity.bind());
	auto transport = COFIBER_AWAIT virtio::discover(std::move(hw_device),
			virtio::DiscoverMode::transitional);

	auto device = new virtio::block::Device{std::move(transport)};
	device->runDevice();

/*
	auto info = COFIBER_AWAIT hw_device.getPciInfo();
	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypePort);
	auto bar = COFIBER_AWAIT hw_device.accessBar(0);
	auto irq = COFIBER_AWAIT hw_device.accessIrq();

	HEL_CHECK(helEnableIo(bar.getHandle()));

	std::cout << "Setting up the device" << std::endl;
	device.setupDevice(info.barInfo[0].address, std::move(irq));
*/
}))

COFIBER_ROUTINE(cofiber::no_future, observeDevices(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-vendor", "1af4"),
		mbus::EqualsFilter("pci-device", "1001")
//		mbus::EqualsFilter("pci-device", "1042")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties) {
		std::cout << "virtio: Detected block device" << std::endl;
		bindDevice(std::move(entity));
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
}))

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting virtio-block driver\n");

	observeDevices();

	while(true)
		helix::Dispatcher::global().dispatch();
}

