#pragma once

#include <arch/mem_space.hpp>
#include <async/basic.hpp>
#include <helix/memory.hpp>
#include <netserver/nic.hpp>
#include <nic/i8254x/rx.hpp>
#include <nic/i8254x/tx.hpp>
#include <nic/i8254x/queue.hpp>
#include <protocols/hw/client.hpp>

constexpr bool logDebug = true;

constexpr int NUM_RX_DESCRIPTORS = 256;
constexpr int NUM_TX_DESCRIPTORS = 256;

struct RxQueue;
struct TxQueue;

struct Intel8254xNic : nic::Link {
	friend RxQueue;
	friend TxQueue;
public:
	Intel8254xNic(protocols::hw::Device device);

	virtual async::result<void> receive(arch::dma_buffer_view) override;
	virtual async::result<void> send(const arch::dma_buffer_view) override;

	async::result<void> init();

private:
	void rxInit();
	void txInit();

	void enableIrqs();
	async::result<uint16_t> eepromRead(uint8_t address);

	async::detached processInterrupt();

	helix::Mapping _mmio_mapping;
	arch::mem_space _mmio;

	arch::contiguous_pool _dmaPool;
	protocols::hw::Device _device;

	std::unique_ptr<RxQueue> _rxQueue;
	std::unique_ptr<TxQueue> _txQueue;

	helix::UniqueDescriptor _irq;
};
