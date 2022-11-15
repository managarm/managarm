#pragma once

#include <arch/mem_space.hpp>
#include <async/basic.hpp>
#include <helix/memory.hpp>
#include <netserver/nic.hpp>
#include <nic/i8254x/rx.hpp>
#include <nic/i8254x/queue.hpp>
#include <protocols/hw/client.hpp>

constexpr bool logDebug = true;

constexpr int NUM_RX_DESCRIPTORS = 256;

struct RxQueue;

struct Intel8254xNic : nic::Link {
	friend RxQueue;
public:
	Intel8254xNic(protocols::hw::Device device);

	async::result<void> init();

private:
	void rxInit();

	void enableIrqs();
	async::result<uint16_t> eepromRead(uint8_t address);

	helix::Mapping _mmio_mapping;
	arch::mem_space _mmio;

	arch::contiguous_pool _dmaPool;
	protocols::hw::Device _device;

	std::unique_ptr<RxQueue> _rxQueue;

	helix::UniqueDescriptor _irq;
};
