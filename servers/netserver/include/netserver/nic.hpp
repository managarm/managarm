#pragma once

#include <arch/dma_pool.hpp>
#include <async/result.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

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

	explicit operator bool() const {
		return !(mac_[0] == 0 && mac_[1] == 0 && mac_[2] == 0 && mac_[3] == 0 && mac_[4] == 0 && mac_[5] == 0);
	}

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
	struct AllocatedBuffer {
		arch::dma_buffer frame;
		arch::dma_buffer_view payload;
	};

	Link(unsigned int mtu, arch::dma_pool *dmaPool);
	virtual ~Link() = default;
	//! Receives an entire frame from the network
	virtual async::result<void> receive(arch::dma_buffer_view) = 0;
	//! Sends an entire ethernet frame
	virtual async::result<void> send(const arch::dma_buffer_view) = 0;
	arch::dma_pool *dmaPool();
	AllocatedBuffer allocateFrame(size_t payloadSize);
	AllocatedBuffer allocateFrame(MacAddress to, EtherType type,
		size_t payloadSize);

	MacAddress deviceMac();
	int index();
	std::string name();
	unsigned int mtu;

	static std::shared_ptr<Link> byIndex(int index);

	static std::unordered_map<int64_t, std::shared_ptr<nic::Link>> &getLinks();
protected:
	arch::dma_pool *dmaPool_;
	MacAddress mac_;
	int index_;
};

async::detached runDevice(std::shared_ptr<Link> dev);
} // namespace nic
