#include <netserver/nic.hpp>

#include <algorithm>
#include <arch/bit.hpp>
#include "ip/ip4.hpp"
#include "ip/arp.hpp"

namespace nic {
uint8_t &MacAddress::operator[](size_t idx) {
	return mac_[idx];
}

const uint8_t &MacAddress::operator[](size_t idx) const {
	return mac_[idx];
}

bool operator==(const MacAddress &l, const MacAddress &r) {
	return std::equal(std::begin(l.mac_), std::end(l.mac_),
		std::begin(r.mac_));
}

bool operator!=(const MacAddress &l, const MacAddress &r) {
	return !operator==(l, r);
}

MacAddress Link::deviceMac() {
	return mac_;
}

arch::dma_pool *Link::dmaPool() {
	return dmaPool_;
}

Link::AllocatedBuffer Link::allocateFrame(MacAddress to, EtherType type,
		size_t payloadSize) {
	// default implementation assume an Ethernet II frame
	using namespace arch;
	Link::AllocatedBuffer buf {
		dma_buffer { dmaPool(), 14 + payloadSize }, {}
	};

	uint16_t et = static_cast<uint16_t>(type);
	et = convert_endian<endian::big>(et);
	std::memcpy(buf.frame.data(), to.data(), sizeof(MacAddress));
	std::memcpy(buf.frame.subview(6).data(), deviceMac().data(),
		sizeof(MacAddress));
	std::memcpy(buf.frame.subview(12).data(), &et, sizeof(et));

	buf.payload = buf.frame.subview(14);
	return buf;
}

async::detached runDevice(std::shared_ptr<nic::Link> dev) {
	using namespace arch;
	while(true) {
		dma_buffer frameBuffer { dev->dmaPool(), 1514 };
		co_await dev->receive(frameBuffer);
		auto capsule = frameBuffer.subview(14);
		auto data = reinterpret_cast<uint8_t*>(frameBuffer.data());
		uint16_t ethertype = data[12] << 8 | data[13];
		nic::MacAddress dstsrc[2];
		std::memcpy(dstsrc, data, sizeof(dstsrc));

		switch (ethertype) {
		case ETHER_TYPE_IP4:
			ip4().feedPacket(dstsrc[0], dstsrc[1],
				std::move(frameBuffer), capsule);
			break;
		case ETHER_TYPE_ARP:
			neigh4().feedArp(dstsrc[0], capsule);
			break;
		default:
			break;
		}
	}
}
} // namespace nic
