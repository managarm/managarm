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

async::detached bindDevice(mbus::Entity entity) {
	protocols::hw::Device hwDevice(co_await entity.bind());
	auto transport = co_await virtio_core::discover(std::move(hwDevice),
			virtio_core::DiscoverMode::transitional);

	auto device = new tty::virtio_console::Device{std::move(transport)};
	device->runDevice();
}

async::detached observeDevices() {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-vendor", "1af4"),
		mbus::EqualsFilter("pci-device", "1003")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties) {
		std::cout << "virtio-console: Detected device" << std::endl;
		bindDevice(std::move(entity));
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("virtio-console: Starting driver\n");

//	HEL_CHECK(helSetPriority(kHelThisThread, 3));

	{
		async::queue_scope scope{helix::globalQueue()};
		observeDevices();
	}

	async::run_forever(helix::globalQueue()->run_token(), helix::currentDispatcher);
}
