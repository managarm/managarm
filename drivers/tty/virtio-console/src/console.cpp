#include <stdlib.h>
#include <string.h>
#include <iostream>

#include "console.hpp"

namespace tty {
namespace virtio_console {

// --------------------------------------------------------
// Device
// --------------------------------------------------------

Device::Device(std::unique_ptr<virtio_core::Transport> transport)
: transport_{std::move(transport)} { }

async::detached Device::runDevice() {
	transport_->finalizeFeatures();
	transport_->claimQueues(2);
	rxQueue_ = transport_->setupQueue(0);
	txQueue_ = transport_->setupQueue(1);

	auto maxPorts = transport_->space().load(spec::regs::maxPorts);
	std::cout << "virtio-console: Device supports " << maxPorts << " ports" << std::endl;

	transport_->runDevice();

	const char *s = "Hello virtio\n";

	arch::dma_buffer buffer{&dmaPool_, strlen(s) + 1};
	memcpy(buffer.data(), s, strlen(s) + 1);

	virtio_core::Chain chain;
	chain.append(co_await txQueue_->obtainDescriptor());
	chain.setupBuffer(virtio_core::hostToDevice, buffer);

	std::cout << "virtio-console: Submitting" << std::endl;
	co_await txQueue_->submitDescriptor(chain.front());
	std::cout << "virtio-console: Done" << std::endl;

}

} } // namespace tty::virtio_console
