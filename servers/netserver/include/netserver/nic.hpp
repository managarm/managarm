#pragma once

#include <arch/dma_pool.hpp>
#include <async/result.hpp>

namespace nic {
struct MacAddress {
	uint8_t &operator[](size_t idx);
	const uint8_t &operator[](size_t idx) const;
private:
	uint8_t mac_[6];
};

// TODO(arsen): Expose interface for csum offloading, constructing frames, and
// other features of NICs
struct Link {
	inline Link(unsigned int mtu) : mtu(mtu) {}
	virtual ~Link() = default;
	//! Receives an entire frame from the network
	virtual async::result<void> receive(arch::dma_buffer_view) = 0;
	//! Sends an entire ethernet frame
	virtual async::result<void> send(const arch::dma_buffer_view) = 0;
	virtual arch::dma_pool *dmaPool() = 0;

	MacAddress deviceMac();
	unsigned int mtu;
protected:
	MacAddress mac_;
};

async::detached runDevice(std::shared_ptr<Link> dev);
} // namespace nic
