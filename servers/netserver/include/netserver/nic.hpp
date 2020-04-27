#pragma once

#include <arch/dma_pool.hpp>
#include <async/result.hpp>
#include <cstdint>

namespace nic {
struct MacAddress {
	uint8_t &operator[](size_t idx);
	const uint8_t &operator[](size_t idx) const;
	inline uint8_t *data() {
		return mac_;
	}
	inline const uint8_t *data() const {
		return mac_;
	}

	friend bool operator==(const MacAddress &l, const MacAddress &r);
	friend bool operator!=(const MacAddress &l, const MacAddress &r);

	inline friend uint8_t *begin(MacAddress &m) {
		return &m.mac_[0];
	}

	inline friend uint8_t *end(MacAddress &m) {
		return &m.mac_[6];
	}
private:
	uint8_t mac_[6] = { 0 };
};

enum EtherType : uint16_t {
	ETHER_TYPE_IP4 = 0x0800,
	ETHER_TYPE_ARP = 0x0806,
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
