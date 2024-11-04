#pragma once

#include <arch/io_space.hpp>
#include <arch/mem_space.hpp>
#include <async/basic.hpp>
#include <helix/memory.hpp>
#include <netserver/nic.hpp>
#include <nic/freebsd-e1000/queue.hpp>
#include <protocols/hw/client.hpp>
#include <queue>

// HACKFIX: FreeBSD's imported e1000 headers do not some with C++ guards, so we improvise them here
extern "C" {

#include <e1000_api.h>
}

constexpr size_t RX_QUEUE_SIZE = 32;
constexpr size_t TX_QUEUE_SIZE = 32;

struct DescriptorSpace {
	uint8_t data[2048];
};

enum class NicType {
	Igb,
	Em,
	Lem,
};

#define em_mac_min e1000_82547
#define igb_mac_min e1000_82575

#define IFF_PROMISC 0x100
#define IFF_ALLMULTI 0x200

struct E1000Nic : nic::Link {
	E1000Nic(protocols::hw::Device device);

	async::result<size_t> receive(arch::dma_buffer_view) override;
	async::result<void> send(const arch::dma_buffer_view) override;

	async::result<void> init();

	void pciRead(u32 reg, u32 *value);
	void pciRead(u32 reg, u16 *value);
	void pciRead(u32 reg, u8 *value);

  private:
	async::detached processIrqs();

	async::result<void> identifyHardware();
	async::result<void> txInit();
	async::result<void> rxInit();

	void em_eth_rx_ack();
	void em_rxd_setup();
	void reap_tx_buffers();

	bool eth_rx_pop();

	int setPromiscuousMode(struct e1000_hw *hw, int flags);

	helix::Mapping _mmio_mapping;
	arch::mem_space _mmio;

	arch::contiguous_pool _dmaPool;
	protocols::hw::Device _device;

	helix::UniqueDescriptor _irq;

	QueueIndex _rxIndex;
	QueueIndex _txIndex;

	arch::dma_array<struct e1000_rx_desc> _rxd;
	arch::dma_array<DescriptorSpace> _rxdbuf;

	arch::dma_array<struct e1000_tx_desc> _txd;
	arch::dma_array<DescriptorSpace> _txdbuf;

	std::queue<Request *> _requests;

  public:
	struct e1000_hw _hw;
	struct e1000_osdep _osdep;

	arch::io_space _io;

	NicType type;
};

void e1000_osdep_set_pci(struct e1000_osdep *st, protocols::hw::Device &pci);

namespace nic::e1000 {

std::shared_ptr<nic::Link> makeShared(protocols::hw::Device device);

}
