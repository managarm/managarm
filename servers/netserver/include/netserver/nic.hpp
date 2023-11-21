#pragma once

#include <arch/dma_pool.hpp>
#include <async/result.hpp>
#include <core/nic/buffer.hpp>
#include <frg/logging.hpp>
#include <frg/formatting.hpp>
#include <helix/ipc.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>

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

struct Link {
	struct AllocatedBuffer {
		nic_core::buffer_view frame;
		nic_core::buffer_view payload;
	};

	Link(helix::UniqueLane lane, unsigned int mtu);
	~Link() = default;
	
	async::result<void> send(const nic_core::buffer_view);
	AllocatedBuffer allocateFrame(MacAddress to, EtherType type,
		size_t payloadSize);

	MacAddress& deviceMac();
	int index();
	std::string name();

	constexpr unsigned int mtu() {
		return _mtu;
	}
	
	async::result<bool> updateMtu(unsigned int mtu, bool ask = true);

	static async::detached runDevice(helix::UniqueLane lane, std::shared_ptr<nic::Link> dev);

protected:
	MacAddress mac_;
	int index_;

	unsigned int _mtu;

	helix::UniqueLane _lane;
};


std::vector<std::shared_ptr<nic::Link>> &getLinks();
std::shared_ptr<Link> byIndex(int index);

async::result<void> registerNic(std::shared_ptr<nic::Link> nic);
async::result<void> unregisterNic(std::shared_ptr<nic::Link> nic);

} // namespace nic

inline std::ostream &operator<<(std::ostream &os, const nic::MacAddress &mac) {
    frg::to(os) << frg::fmt("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return os;
}
