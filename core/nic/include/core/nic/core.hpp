#pragma once

#include <arch/dma_pool.hpp>
#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <protocols/mbus/client.hpp>

namespace nic_core {

struct Nic {
	virtual async::result<size_t> receive(arch::dma_buffer_view) = 0;
	virtual async::result<void> send(const arch::dma_buffer_view) = 0;

	Nic(arch::dma_pool *dmaPool);

	virtual ~Nic() = default;

	constexpr unsigned int mtu() {
		return mtu_;
	}

	async::result<void> updateMtu(unsigned int new_mtu);

	std::vector<uint8_t> mac;

	async::result<void> startDevice(helix::UniqueLane packet_recv_lane, helix::UniqueLane packet_send_lane);
	static async::result<void> doBind(helix::UniqueLane &netserverLane, mbus::Entity base_entity, std::shared_ptr<nic_core::Nic> dev);
private:
	async::detached doSendPackets();
	async::detached doRecvPackets();

	helix::UniqueLane to_netserver;
	helix::UniqueLane from_netserver;

	arch::dma_pool *dmaPool_;

	// Check if a MTU is allowed by the NIC.
	virtual async::result<bool> verifyMtu(size_t requestedMtu) {
		co_return false;
	}

	unsigned int mtu_;
};



} // namespace nic_core
