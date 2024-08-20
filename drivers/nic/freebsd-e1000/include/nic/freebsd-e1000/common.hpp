#pragma once

#include <async/basic.hpp>
#include <helix/memory.hpp>
#include <netserver/nic.hpp>
#include <nic/freebsd-e1000/queue.hpp>
#include <protocols/hw/client.hpp>

constexpr size_t RX_QUEUE_SIZE = 32;
constexpr size_t TX_QUEUE_SIZE = 32;

struct E1000Nic : nic::Link {
	E1000Nic(protocols::hw::Device device);

	async::result<size_t> receive(arch::dma_buffer_view) override;
	async::result<void> send(const arch::dma_buffer_view) override;

	async::result<void> init();

private:
	arch::contiguous_pool _dmaPool;
	protocols::hw::Device _device;

	helix::UniqueDescriptor _irq;

	QueueIndex _rxIndex;
	QueueIndex _txIndex;
};

namespace nic::e1000 {

std::shared_ptr<nic::Link> makeShared(protocols::hw::Device device);

}
