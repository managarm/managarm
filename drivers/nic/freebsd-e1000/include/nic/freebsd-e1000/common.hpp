#pragma once

#include <async/basic.hpp>
#include <netserver/nic.hpp>
#include <protocols/hw/client.hpp>
#include <nic/freebsd-e1000/queue.hpp>

struct E1000Nic : nic::Link {
	E1000Nic(protocols::hw::Device device);

	virtual async::result<void> receive(arch::dma_buffer_view) override;
	virtual async::result<void> send(const arch::dma_buffer_view) override;

	async::result<void> init();

private:
	protocols::hw::Device _device;

	helix::UniqueDescriptor _irq;

	QueueIndex _rxIndex;
	QueueIndex _txIndex;
};

namespace nic::e1000 {

std::shared_ptr<nic::Link> makeShared(protocols::hw::Device device);

}
