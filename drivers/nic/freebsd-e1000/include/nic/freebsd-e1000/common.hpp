#pragma once

#include <arch/io_space.hpp>
#include <arch/mem_space.hpp>
#include <async/basic.hpp>
#include <helix/memory.hpp>
#include <netserver/nic.hpp>
#include <nic/freebsd-e1000/queue.hpp>
#include <protocols/hw/client.hpp>

// HACKFIX: FreeBSD's imported e1000 headers do not some with C++ guards, so we improvise them here
extern "C" {

#include <e1000_api.h>

}

constexpr size_t RX_QUEUE_SIZE = 32;
constexpr size_t TX_QUEUE_SIZE = 32;

struct E1000Nic : nic::Link {
	E1000Nic(protocols::hw::Device device);

	async::result<size_t> receive(arch::dma_buffer_view) override;
	async::result<void> send(const arch::dma_buffer_view) override;

	async::result<void> init();

	void pciRead(u32 reg, u32 *value);
	void pciRead(u32 reg, u16 *value);
	void pciRead(u32 reg, u8 *value);

private:
	arch::contiguous_pool _dmaPool;
	protocols::hw::Device _device;

	helix::UniqueDescriptor _irq;

	QueueIndex _rxIndex;
	QueueIndex _txIndex;

public:
	struct e1000_hw _hw;

	arch::io_space _io;
};

namespace nic::e1000 {

std::shared_ptr<nic::Link> makeShared(protocols::hw::Device device);

}
