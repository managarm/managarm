#include <stdlib.h>
#include <iostream>

#include <helix/await.hpp>

#include "net.hpp"

namespace nic {
namespace virtio {

static uint8_t deviceMac[6];

static async::result<void> testNetworking(Device *device);

// --------------------------------------------------------
// Device
// --------------------------------------------------------

Device::Device(std::unique_ptr<virtio_core::Transport> transport)
: _transport{std::move(transport)},
		_receiveVq{nullptr}, _transmitVq{nullptr} { }

void Device::runDevice() {
	uint8_t mac[6];
	if(_transport->checkDeviceFeature(VIRTIO_NET_F_MAC)) {
		for (int i = 0; i < 6; i++)
			mac[i] = _transport->loadConfig8(i);
		memcpy(deviceMac, mac, 6);
		char ms[3 * 6 + 1];
		sprintf(ms, "%.2x-%.2x-%.2x-%.2x-%.2x-%.2x",
				mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		std::cout << "nic-virtio: Device has a hardware MAC: " << ms << std::endl;
		_transport->acknowledgeDriverFeature(VIRTIO_NET_F_MAC);
	}

	_transport->finalizeFeatures();
	_transport->claimQueues(2);
	_receiveVq = _transport->setupQueue(0);
	_transmitVq = _transport->setupQueue(1);

	_transport->runDevice();

	_processReceive();

	async::detach(testNetworking(this));
}

async::result<void> Device::sendPacket(const std::vector<std::byte> &payload) {
	if(payload.size() > 1514)
		throw std::runtime_error("Packet exceeds maximum ethernet size");

	arch::dma_object<VirtHeader> header{&_dmaPool};
	arch::dma_buffer packet{&_dmaPool, payload.size()};

	memset(header.data(), 0, sizeof(VirtHeader));
	memcpy(packet.data(), payload.data(), payload.size());

	virtio_core::Chain chain;
	chain.append(co_await _transmitVq->obtainDescriptor());
	chain.setupBuffer(virtio_core::hostToDevice,
			header.view_buffer().subview(0, legacyHeaderSize));
	chain.append(co_await _transmitVq->obtainDescriptor());
	chain.setupBuffer(virtio_core::hostToDevice, packet);

	std::cout << "nic-virtio: Preparing to send" << std::endl;
	co_await _transmitVq->submitDescriptor(chain.front());
	std::cout << "nic-virtio: Sent packet" << std::endl;
}

async::detached Device::_processReceive() {
	while(true) {
		arch::dma_object<VirtHeader> header{&_dmaPool};
		arch::dma_buffer packet{&_dmaPool, 1514};

		virtio_core::Chain chain;
		chain.append(co_await _receiveVq->obtainDescriptor());
		chain.setupBuffer(virtio_core::deviceToHost,
				header.view_buffer().subview(0, legacyHeaderSize));
		chain.append(co_await _receiveVq->obtainDescriptor());
		chain.setupBuffer(virtio_core::deviceToHost, packet);

		std::cout << "nic-virtio: Preparing to receive" << std::endl;
		co_await _receiveVq->submitDescriptor(chain.front());
		std::cout << "nic-virtio: Received packet" << std::endl;
	}
}

// --------------------------------------------------------

template<typename T>
T hostToNet(T value);

template<typename T>
T netToHost(T value);

template<>
inline uint16_t hostToNet(uint16_t value) {
	return __builtin_bswap16(value);
}
template<>
inline uint32_t hostToNet(uint32_t value) {
	return __builtin_bswap32(value);
}

template<>
inline uint16_t netToHost(uint16_t value) {
	return __builtin_bswap16(value);
}
template<>
inline uint32_t netToHost(uint32_t value) {
	return __builtin_bswap32(value);
}

// Ethernet constants and structs.

enum {
	kEtherIp4 = 0x0800,
	kEtherArp = 0x0806
};

struct MacAddress {
	static MacAddress broadcast() {
		return MacAddress(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);
	}

	MacAddress() : octets{ 0, 0, 0, 0, 0, 0 } { }

	MacAddress(uint8_t octet0, uint8_t octet1, uint8_t octet2,
			uint8_t octet3, uint8_t octet4, uint8_t octet5)
	: octets{ octet0, octet1, octet2, octet3, octet4, octet5 } { }

	bool operator== (const MacAddress &other) {
		return memcmp(octets, other.octets, 6) == 0;
	}
	bool operator!= (const MacAddress &other) {
		return !(*this == other);
	}

	uint8_t octets[6];
};

struct EthernetInfo {
	MacAddress destMac;
	MacAddress sourceMac;
	uint16_t etherType;
};

struct EthernetHeader {
	MacAddress destAddress;
	MacAddress sourceAddress;
	uint16_t etherType;
};

// IP4 constants and structs.

enum {
	kIp4Version = 4,
	kIp6Version = 6,
	kTtl = 64,
	kUdpProtocol = 17,
	kTcpProtocol = 6,

	kFlagReserved = 0x8000,
	kFlagDF = 0x4000,
	kFlagMF = 0x2000,
	kFragmentOffsetMask = 0x1FFF
};

struct Ip4Address {
	static Ip4Address broadcast() {
		return Ip4Address(0xFF, 0xFF, 0xFF, 0xFF);
	}

	Ip4Address() : octets{ 0, 0, 0, 0 } { }

	Ip4Address(uint8_t octet0, uint8_t octet1, uint8_t octet2,
			uint8_t octet3)
	: octets{ octet0, octet1, octet2, octet3 } { }

	Ip4Address(uint32_t word)
	: octets{ uint8_t(word >> 24), uint8_t((word >> 16) & 0xFF),
			uint8_t((word >> 8) & 0xFF), uint8_t(word & 0xFF) } { }

	bool operator== (const Ip4Address &other) const {
		return memcmp(octets, other.octets, 4) == 0;
	}
	bool operator!= (const Ip4Address &other) const {
		return !(*this == other);
	}

	uint8_t octets[4];
};

struct Ip4Info {
	Ip4Address sourceIp;
	Ip4Address destIp;
	uint8_t protocol;
};

struct Ip4Header {
	uint8_t version_headerLength;
	uint8_t dscp_ecn;
	uint16_t length;
	uint16_t identification;
	uint16_t flags_offset;
	uint8_t ttl;
	uint8_t protocol;
	uint16_t checksum;
	Ip4Address sourceIp;
	Ip4Address targetIp;
};
static_assert(sizeof(Ip4Header) == 20, "Bad sizeof(Ip4Header)");

struct PseudoIp4Header {
	uint8_t sourceIp[4];
	uint8_t destIp[4];
	uint8_t reserved;
	uint8_t protocol;
	uint16_t length;
};
static_assert(sizeof(PseudoIp4Header) == 12, "Bad sizeof(PseudoIp4Header)");

struct Checksum {
	Checksum()
	: currentSum(0) { }

	void update(const void *buffer, size_t size) {
		auto bytes = reinterpret_cast<const unsigned char *>(buffer);

		if(size == 0)
			return;

		size_t i;
		for(i = 0; i < size - 1; i += 2) {
			uint16_t high = bytes[i], low = bytes[i + 1];
			update((high << 8) | low);
		}
		if(size % 2)
			update(bytes[i]);
	}

	void update(uint16_t value) {
		currentSum += value;
	}

	uint16_t finish() {
		uint32_t result = currentSum;
		while (result >> 16)
			result = (result & 0xFFFF) + (result >> 16);
		assert(result != 0 && result != 0xFFFF); // FIXME: fix this case
		return ~result;
	}

private:
	uint32_t currentSum;
};

// UDP constants and structs.

struct UdpInfo {
	uint16_t sourcePort;
	uint16_t destPort;
};

struct UdpHeader {
	uint16_t source;
	uint16_t destination;
	uint16_t length;
	uint16_t checksum;
};

// DHCP constants and structs.

enum {
	kBootpNull = 0,
	kBootpEnd = 255,

	kBootpSubnet = 1,
	kBootpRouters = 3,
	kBootpDns = 6,

	kDhcpRequestedIp = 50,
	kDhcpLeaseTime = 51,
	kDhcpMessageType = 53,
	kDhcpServer = 54
};

enum {
	kTypeDiscover = 1,
	kTypeOffer = 2,
	kTypeRequest = 3,
	kTypeDecline = 4,
	kTypeAck = 5,
	kTypeNak = 6,
	kTypeRelease = 7,
	kTypeInform = 8
};

enum {
	// bits of the BOOTP flags field
	kDhcpBroadcast = 0x8000,

	// dhcp magic option
	kDhcpMagic = 0x63825363
};

struct DhcpHeader {
	uint8_t op;
	uint8_t htype;
	uint8_t hlen;
	uint8_t hops;
	uint32_t transaction;
	uint16_t secondsSinceBoot;
	uint16_t flags;
	Ip4Address clientIp;
	Ip4Address assignedIp;
	Ip4Address serverIp;
	Ip4Address gatewayIp;
	uint8_t clientHardware[16];
	uint8_t serverHost[64];
	uint8_t file[128];
	uint32_t magic; // move this out of DhcpHeader
};

static async::result<void> sendEthernetPacket(Device *device,
		EthernetInfo link_info,
		const std::vector<std::byte> &payload) {
	EthernetHeader header;
	header.destAddress = link_info.destMac;
	header.sourceAddress = link_info.sourceMac;
	header.etherType = hostToNet<uint16_t>(link_info.etherType);

	std::vector<std::byte> packet(sizeof(EthernetHeader) + payload.size());
	memcpy(&packet[0], &header, sizeof(EthernetHeader));
	memcpy(&packet[sizeof(EthernetHeader)], payload.data(), payload.size());

	co_await device->sendPacket(packet);
}

static async::result<void> sendIp4Packet(Device *device,
		EthernetInfo link_info, Ip4Info network_info,
		const std::vector<std::byte> &payload) {
	Ip4Header header;
	header.version_headerLength = (kIp4Version << 4) | (sizeof(Ip4Header) / 4);
	header.dscp_ecn = 0;
	header.length = hostToNet<uint16_t>(sizeof(Ip4Header) + payload.size());
	header.identification = hostToNet<uint16_t>(666);
	header.flags_offset = 0;
	header.ttl = kTtl;
	header.protocol = network_info.protocol;
	header.checksum = 0;
	header.sourceIp = network_info.sourceIp;
	header.targetIp = network_info.destIp;

	Checksum checksum;
	checksum.update(&header, sizeof(Ip4Header));
	header.checksum = hostToNet<uint16_t>(checksum.finish());

	std::vector<std::byte> packet(sizeof(Ip4Header) + payload.size());
	memcpy(&packet[0], &header, sizeof(Ip4Header));
	memcpy(&packet[sizeof(Ip4Header)], payload.data(), payload.size());

	co_await sendEthernetPacket(device, link_info, packet);
}

static async::result<void> sendUdpPacket(Device *device,
		EthernetInfo link_info, Ip4Info network_info,  UdpInfo transport_info,
		const std::vector<std::byte> &payload) {
	UdpHeader header;
	header.source = hostToNet<uint16_t>(transport_info.sourcePort);
	header.destination = hostToNet<uint16_t>(transport_info.destPort);
	header.length = hostToNet<uint16_t>(sizeof(UdpHeader) + payload.size());
	header.checksum = 0;

	// Calculate the UDP checksum.
	PseudoIp4Header pseudo;
	memcpy(pseudo.sourceIp, network_info.sourceIp.octets, 4);
	memcpy(pseudo.destIp, network_info.destIp.octets, 4);
	pseudo.reserved = 0;
	pseudo.protocol = kUdpProtocol;
	pseudo.length = hostToNet<uint16_t>(sizeof(UdpHeader) + payload.size());

	Checksum udp_checksum;
	udp_checksum.update(&pseudo, sizeof(PseudoIp4Header));
	udp_checksum.update(&header, sizeof(UdpHeader));
	udp_checksum.update(payload.data(), payload.size());
	header.checksum = hostToNet<uint16_t>(udp_checksum.finish());

	std::vector<std::byte> packet(sizeof(UdpHeader) + payload.size());
	memcpy(&packet[0], &header, sizeof(UdpHeader));
	memcpy(&packet[sizeof(UdpHeader)], payload.data(), payload.size());

	co_await sendIp4Packet(device, link_info, network_info, packet);
}

static uint32_t dhcpTransaction = 0xD61FF088; // Some random integer.

static async::result<void> sendDhcpDiscover(Device *device) {
	// Send a DHCP DISCOVER packet.
	MacAddress local_mac{deviceMac[0], deviceMac[1], deviceMac[2],
			deviceMac[3], deviceMac[4], deviceMac[5]};

	std::vector<std::byte> packet;
	packet.resize(sizeof(DhcpHeader) + 4);

	DhcpHeader dhcp_header;
	dhcp_header.op = 1;
	dhcp_header.htype = 1;
	dhcp_header.hlen = 6;
	dhcp_header.hops = 0;
	dhcp_header.transaction = hostToNet<uint32_t>(dhcpTransaction);
	dhcp_header.secondsSinceBoot = 0;
	dhcp_header.flags = hostToNet<uint16_t>(kDhcpBroadcast);
	dhcp_header.clientIp = Ip4Address();
	dhcp_header.assignedIp = Ip4Address();
	dhcp_header.serverIp = Ip4Address();
	dhcp_header.gatewayIp = Ip4Address();
	memset(dhcp_header.clientHardware, 0, 16);
	memcpy(dhcp_header.clientHardware, local_mac.octets, 6);
	memset(dhcp_header.serverHost, 0, 64);
	memset(dhcp_header.file, 0, 128);
	dhcp_header.magic = hostToNet<uint32_t>(kDhcpMagic);
	memcpy(&packet[0], &dhcp_header, sizeof(DhcpHeader));

	auto dhcp_options = &packet[sizeof(DhcpHeader)];
	dhcp_options[0] = static_cast<std::byte>(kDhcpMessageType);
	dhcp_options[1] = static_cast<std::byte>(1);
	dhcp_options[2] = static_cast<std::byte>(kTypeDiscover);
	dhcp_options[3] = static_cast<std::byte>(kBootpEnd);

	EthernetInfo ethernet_info;
	ethernet_info.sourceMac = local_mac;
	ethernet_info.destMac = MacAddress::broadcast();
	ethernet_info.etherType = kEtherIp4;

	Ip4Info ip_info;
	ip_info.sourceIp = Ip4Address();
	ip_info.destIp = Ip4Address::broadcast();
	ip_info.protocol = kUdpProtocol;

	UdpInfo udp_info;
	udp_info.sourcePort = 68;
	udp_info.destPort = 67;

	co_await sendUdpPacket(device, ethernet_info, ip_info, udp_info, packet);
}

static async::result<void> testNetworking(Device *device) {
	while(true) {
		uint64_t tick;
		HEL_CHECK(helGetClock(&tick));

		helix::AwaitClock await_clock;
		auto &&submit = helix::submitAwaitClock(&await_clock, tick + 2'000'000'000,
				helix::Dispatcher::global());
		co_await submit.async_wait();
		HEL_CHECK(await_clock.error());

		async::detach(sendDhcpDiscover(device));
	}
}

} } // namespace nic::virtio
