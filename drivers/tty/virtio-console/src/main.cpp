#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include <async/result.hpp>
#include <hel.h>
#include <hel-syscalls.h>
#include <helix/ipc.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/hw/client.hpp>

#include "console.hpp"

async::detached bindDevice(mbus_ng::Entity hwEntity) {
	protocols::hw::Device hwDevice((co_await hwEntity.getRemoteLane()).unwrap());
	co_await hwDevice.enableBusmaster();
	auto transport = co_await virtio_core::discover(std::move(hwDevice),
			virtio_core::DiscoverMode::transitional);

	auto device = new tty::virtio_console::Device{std::move(transport)};
	device->runDevice();
}

async::detached observeDevices() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"pci-vendor", "1af4"},
		mbus_ng::EqualsFilter{"pci-device", "1003"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			std::cout << "virtio-console: Detected controller" << std::endl;
			bindDevice(std::move(entity));
		}
	}
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("virtio-console: Starting driver\n");

	HEL_CHECK(helSetPriority(kHelThisThread, 1));

	observeDevices();
	async::run_forever(helix::currentDispatcher);
}
