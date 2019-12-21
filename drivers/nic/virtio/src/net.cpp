#include <stdlib.h>
#include <iostream>

#include "net.hpp"

namespace nic {
namespace virtio {

static uint8_t deviceMac[6];

static async::result<void> testNetworking(Device *device);
static void recvEthernetPacket(Device *device,
		arch::dma_buffer_view buffer);
static void recvIp4Packet(Device *device,
		arch::dma_buffer_view buffer);
static void recvUdpPacket(Device *device,
		arch::dma_buffer_view buffer);

static std::deque<std::vector<std::byte>> dhcpInPackets;
static async::doorbell dhcpInDoorbell;

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

		recvEthernetPacket(this, packet);
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

static void recvEthernetPacket(Device *device,
		arch::dma_buffer_view buffer) {
	if(buffer.size() < sizeof(EthernetHeader)) {
		std::cout << "nic-virtio: Ethernet packet without header" << std::endl;
		return;
	}

	EthernetHeader header;
	memcpy(&header, buffer.data(), sizeof(EthernetHeader));

	if(header.etherType == hostToNet<uint16_t>(kEtherIp4))
		recvIp4Packet(device, buffer.subview(sizeof(EthernetHeader)));
}

static void recvIp4Packet(Device *device,
		arch::dma_buffer_view buffer) {
	if(buffer.size() < sizeof(Ip4Header)) {
		std::cout << "nic-virtio: IP4 packet without header" << std::endl;
		return;
	}

	Ip4Header header;
	memcpy(&header, buffer.data(), sizeof(Ip4Header));

	size_t header_length = (header.version_headerLength & 0xF) * 4;
	if(buffer.size() < sizeof(Ip4Header)) {
		std::cout << "nic-virtio: IP4 packet smaller than length of header" << std::endl;
		return;
	}

	// TODO: Support fragment reassembly.
	auto fo = netToHost<uint16_t>(header.flags_offset);
	if((fo & kFragmentOffsetMask) || (fo & kFlagMF)) {
		std::cout << "nic-virtio: Dropping fragmented IP4 packet" << std::endl;
		return;
	}

	if(header.protocol == kUdpProtocol) {
		recvUdpPacket(device, buffer.subview(header_length));
	}else{
		std::cout << "nic-virtio: Dropping unexpected IP4 protocol "
				<< (int)header.protocol << std::endl;
	}
}

static void recvUdpPacket(Device *device,
		arch::dma_buffer_view buffer) {
	std::cout << "nic-virtio: Got UDP packet" << std::endl;

	if(buffer.size() < sizeof(UdpHeader)) {
		std::cout << "nic-virtio: UDP packet without header" << std::endl;
		return;
	}

	UdpHeader header;
	memcpy(&header, buffer.data(), sizeof(UdpHeader));

	if(buffer.size() < sizeof(UdpHeader) + netToHost<uint16_t>(header.length)) {
		std::cout << "nic-virtio: UDP packet is smaller than advertised" << std::endl;
		return;
	}

	auto port = netToHost<uint16_t>(header.destination);
	if(port == 68) {
		std::vector<std::byte> payload;
		payload.resize(netToHost<uint16_t>(header.length));
		memcpy(payload.data(), buffer.subview(sizeof(UdpHeader)).data(), payload.size());

		dhcpInPackets.push_back(std::move(payload));
		dhcpInDoorbell.ring();
	}else{
		std::cout << "nic-virtio: Dropping unexpected IP4 protocol "
				<< port << std::endl;
	}
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

// --------------------------------------------------------

// DHCP constants and structs.

namespace spec {

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

} // namespace spec

template<typename F>
void walkBootpOptions(const std::vector<std::byte> &buffer, F functor) {
	size_t offset = 0;
	auto options = buffer.data() + sizeof(spec::DhcpHeader);
	size_t length = buffer.size() - sizeof(spec::DhcpHeader);
	while(offset < length) {
		auto tag = static_cast<int>(options[offset]);
		if(tag == spec::kBootpNull) {
			continue;
		}else if(tag == spec::kBootpEnd) {
			break;
		}

		if(offset + 2 > length) {
			std::cout << "nic-virtio: DHCP option at end of packet" << std::endl;
			return;
		}

		auto opt_size = static_cast<size_t>(options[offset + 1]);
		if(offset + 2 + opt_size > length) {
			std::cout << "nic-virtio: DHCP option overflows packet size" << std::endl;
			return;
		}

		functor(tag, options + offset + 2, opt_size);
		offset += 2 + opt_size;
	}
}

void dumpBootpPacket(const std::vector<std::byte> &buffer) {
	assert(buffer.size() >= sizeof(spec::DhcpHeader));
	spec::DhcpHeader dhcp_header;
	memcpy(&dhcp_header, buffer.data(), sizeof(spec::DhcpHeader));

	std::cout << "nic-virtio: Dumping BOOTP packet" << std::endl;
	printf("    BOOTP operation: %d\n", dhcp_header.op);
	auto bootp_client_ip = dhcp_header.clientIp;
	auto bootp_assigned_ip = dhcp_header.assignedIp;
	auto bootp_server_ip = dhcp_header.serverIp;
	auto bootp_gateway_ip = dhcp_header.gatewayIp;
	printf("    BOOTP clientIp: %d.%d.%d.%d, assignedIp: %d.%d.%d.%d\n",
			bootp_client_ip.octets[0], bootp_client_ip.octets[1],
			bootp_client_ip.octets[2], bootp_client_ip.octets[3],
			bootp_assigned_ip.octets[0], bootp_assigned_ip.octets[1],
			bootp_assigned_ip.octets[2], bootp_assigned_ip.octets[3]);
	printf("    BOOTP serverIp: %d.%d.%d.%d, gatewayIp: %d.%d.%d.%d\n",
			bootp_server_ip.octets[0], bootp_server_ip.octets[1],
			bootp_server_ip.octets[2], bootp_server_ip.octets[3],
			bootp_gateway_ip.octets[0], bootp_gateway_ip.octets[1],
			bootp_gateway_ip.octets[2], bootp_gateway_ip.octets[3]);

	walkBootpOptions(buffer, [&] (int tag, const std::byte *opt_data, size_t opt_size) {
		switch(tag) {
		case spec::kDhcpMessageType:
			assert(opt_size == 1);
			printf("    DHCP messageType: %d\n", static_cast<int>(*opt_data));
			break;
		case spec::kDhcpServer:
		case spec::kBootpSubnet:
		case spec::kBootpRouters:
		case spec::kBootpDns:
		case spec::kDhcpLeaseTime:
			break;
		default:
			printf("    Unexpected BOOTP option: %d !\n", tag);
		}
	});
}

static uint32_t dhcpTransaction = 0xD61FF088; // Some random integer.

static constexpr int nDhcpRetries = 5;

Ip4Address dhcpIp;
Ip4Address assignedIp;
Ip4Address routerIp;
Ip4Address subnetMask;
Ip4Address dnsIp;

// Send a DHCP DISCOVER packet.
static async::result<void> sendDhcpDiscover(Device *device) {
	MacAddress local_mac{deviceMac[0], deviceMac[1], deviceMac[2],
			deviceMac[3], deviceMac[4], deviceMac[5]};

	std::vector<std::byte> packet;
	packet.resize(sizeof(spec::DhcpHeader) + 4);

	spec::DhcpHeader dhcp_header;
	dhcp_header.op = 1;
	dhcp_header.htype = 1;
	dhcp_header.hlen = 6;
	dhcp_header.hops = 0;
	dhcp_header.transaction = hostToNet<uint32_t>(dhcpTransaction);
	dhcp_header.secondsSinceBoot = 0;
	dhcp_header.flags = hostToNet<uint16_t>(spec::kDhcpBroadcast);
	dhcp_header.clientIp = Ip4Address();
	dhcp_header.assignedIp = Ip4Address();
	dhcp_header.serverIp = Ip4Address();
	dhcp_header.gatewayIp = Ip4Address();
	memset(dhcp_header.clientHardware, 0, 16);
	memcpy(dhcp_header.clientHardware, local_mac.octets, 6);
	memset(dhcp_header.serverHost, 0, 64);
	memset(dhcp_header.file, 0, 128);
	dhcp_header.magic = hostToNet<uint32_t>(spec::kDhcpMagic);
	memcpy(&packet[0], &dhcp_header, sizeof(spec::DhcpHeader));

	auto dhcp_options = &packet[sizeof(spec::DhcpHeader)];
	dhcp_options[0] = static_cast<std::byte>(spec::kDhcpMessageType);
	dhcp_options[1] = static_cast<std::byte>(1);
	dhcp_options[2] = static_cast<std::byte>(spec::kTypeDiscover);
	dhcp_options[3] = static_cast<std::byte>(spec::kBootpEnd);

	EthernetInfo ethernet_info;
	ethernet_info.sourceMac = local_mac;
	ethernet_info.destMac = MacAddress::broadcast();
	ethernet_info.etherType = kEtherIp4;

	Ip4Info ip_info;
	ip_info.sourceIp = Ip4Address{};
	ip_info.destIp = Ip4Address::broadcast();
	ip_info.protocol = kUdpProtocol;

	UdpInfo udp_info;
	udp_info.sourcePort = 68;
	udp_info.destPort = 67;

	co_await sendUdpPacket(device, ethernet_info, ip_info, udp_info, packet);
}

// Send a DHCP REQUEST packet.
static async::result<void> sendDhcpRequest(Device *device) {
	MacAddress local_mac{deviceMac[0], deviceMac[1], deviceMac[2],
			deviceMac[3], deviceMac[4], deviceMac[5]};

	std::vector<std::byte> packet;
	packet.resize(sizeof(spec::DhcpHeader) + 16);

	spec::DhcpHeader new_dhcp_header;
	new_dhcp_header.op = 1;
	new_dhcp_header.htype = 1;
	new_dhcp_header.hlen = 6;
	new_dhcp_header.hops = 0;
	new_dhcp_header.transaction = hostToNet<uint32_t>(dhcpTransaction);
	new_dhcp_header.secondsSinceBoot = 0;
	new_dhcp_header.flags = 0;
	new_dhcp_header.clientIp = Ip4Address();
	new_dhcp_header.assignedIp = Ip4Address();
	new_dhcp_header.serverIp = dhcpIp;
	new_dhcp_header.gatewayIp = Ip4Address();
	memset(new_dhcp_header.clientHardware, 0, 16);
	memcpy(new_dhcp_header.clientHardware, local_mac.octets, 6);
	memset(new_dhcp_header.serverHost, 0, 64);
	memset(new_dhcp_header.file, 0, 128);
	new_dhcp_header.magic = hostToNet<uint32_t>(spec::kDhcpMagic);
	memcpy(&packet[0], &new_dhcp_header, sizeof(spec::DhcpHeader));

	auto dhcp_options = &packet[sizeof(spec::DhcpHeader)];
	dhcp_options[0] = static_cast<std::byte>(spec::kDhcpMessageType);
	dhcp_options[1] = static_cast<std::byte>(1);
	dhcp_options[2] = static_cast<std::byte>(spec::kTypeRequest);
	dhcp_options[3] = static_cast<std::byte>(spec::kDhcpServer);
	dhcp_options[4] = static_cast<std::byte>(4);
	dhcp_options[5] = static_cast<std::byte>(dhcpIp.octets[0]);
	dhcp_options[6] = static_cast<std::byte>(dhcpIp.octets[1]);
	dhcp_options[7] = static_cast<std::byte>(dhcpIp.octets[2]);
	dhcp_options[8] = static_cast<std::byte>(dhcpIp.octets[3]);
	dhcp_options[9] = static_cast<std::byte>(spec::kDhcpRequestedIp);
	dhcp_options[10] = static_cast<std::byte>(4);
	dhcp_options[11] = static_cast<std::byte>(assignedIp.octets[0]);
	dhcp_options[12] = static_cast<std::byte>(assignedIp.octets[1]);
	dhcp_options[13] = static_cast<std::byte>(assignedIp.octets[2]);
	dhcp_options[14] = static_cast<std::byte>(assignedIp.octets[3]);
	dhcp_options[15] = static_cast<std::byte>(spec::kBootpEnd);

	EthernetInfo ethernet_info;
	ethernet_info.sourceMac = local_mac;
	ethernet_info.destMac = MacAddress::broadcast();
	ethernet_info.etherType = kEtherIp4;

	Ip4Info ip_info;
	ip_info.sourceIp = Ip4Address{};
	ip_info.destIp = Ip4Address::broadcast();
	ip_info.protocol = kUdpProtocol;

	UdpInfo udp_info;
	udp_info.sourcePort = 68;
	udp_info.destPort = 67;

	co_await sendUdpPacket(device, ethernet_info, ip_info, udp_info, packet);
}

static async::result<bool> dhcpDiscover(Device *device) {
	MacAddress local_mac{deviceMac[0], deviceMac[1], deviceMac[2],
			deviceMac[3], deviceMac[4], deviceMac[5]};

	for(int n = 0; n < nDhcpRetries; n++) {
		std::cout << "nic-virtio: Sending DHCP DISCOVER" << std::endl;
		co_await sendDhcpDiscover(device);

		while(true) {
			// TODO: break out of this loop once x seconds have passed.

			if(dhcpInPackets.empty()) {
				co_await dhcpInDoorbell.async_wait();
				continue;
			}

			std::cout << "nic-virtio: Received DHCP packet" << std::endl;
			auto buffer = std::move(dhcpInPackets.front());
			dhcpInPackets.pop_front();

			if(buffer.size() < sizeof(spec::DhcpHeader)) {
				std::cout << "nic-virtio: Discarding DHCP packet with truncated header"
						<< std::endl;
				continue;
			}

			dumpBootpPacket(buffer);

			spec::DhcpHeader dhcp_header;
			memcpy(&dhcp_header, buffer.data(), sizeof(spec::DhcpHeader));

			int dhcp_type;
			walkBootpOptions(buffer, [&] (int tag, const std::byte *opt_data, size_t opt_size) {
				switch(tag) {
				case spec::kDhcpMessageType:
					assert(opt_size == 1);
					dhcp_type = static_cast<int>(*opt_data);
					break;
				case spec::kDhcpServer:
					assert(opt_size == 4);
					dhcpIp = Ip4Address{static_cast<uint8_t>(opt_data[0]),
							static_cast<uint8_t>(opt_data[1]),
							static_cast<uint8_t>(opt_data[2]),
							static_cast<uint8_t>(opt_data[3])};
					break;
				case spec::kBootpSubnet:
					assert(opt_size == 4);
					subnetMask = Ip4Address{static_cast<uint8_t>(opt_data[0]),
							static_cast<uint8_t>(opt_data[1]),
							static_cast<uint8_t>(opt_data[2]),
							static_cast<uint8_t>(opt_data[3])};
					break;
				case spec::kBootpRouters:
					assert(opt_size == 4);
					routerIp = Ip4Address{static_cast<uint8_t>(opt_data[0]),
							static_cast<uint8_t>(opt_data[1]),
							static_cast<uint8_t>(opt_data[2]),
							static_cast<uint8_t>(opt_data[3])};
					break;
				case spec::kBootpDns:
					assert(opt_size == 4);
					dnsIp = Ip4Address{static_cast<uint8_t>(opt_data[0]),
							static_cast<uint8_t>(opt_data[1]),
							static_cast<uint8_t>(opt_data[2]),
							static_cast<uint8_t>(opt_data[3])};
					break;
				case spec::kDhcpLeaseTime:
					break;
				default:
					printf("    Unexpected BOOTP option: %d !\n", tag);
				}
			});

			if(dhcp_type != spec::kTypeOffer) {
				std::cout << "nic-virtio: Discarding DHCP packet of unexpected type"
						<< std::endl;
				continue;
			}
			co_return true;
		}
	}

	co_return false;
}

static async::result<bool> dhcpRequest(Device *device) {
	MacAddress local_mac{deviceMac[0], deviceMac[1], deviceMac[2],
			deviceMac[3], deviceMac[4], deviceMac[5]};

	for(int n = 0; n < nDhcpRetries; n++) {
		std::cout << "nic-virtio: Sending DHCP REQUEST" << std::endl;
		co_await sendDhcpRequest(device);

		while(true) {
			// TODO: break out of this loop once x seconds have passed.

			if(dhcpInPackets.empty()) {
				co_await dhcpInDoorbell.async_wait();
				continue;
			}

			std::cout << "nic-virtio: Received DHCP packet" << std::endl;
			auto buffer = std::move(dhcpInPackets.front());
			dhcpInPackets.pop_front();

			if(buffer.size() < sizeof(spec::DhcpHeader)) {
				std::cout << "nic-virtio: Discarding DHCP packet with truncated header"
						<< std::endl;
				continue;
			}

			dumpBootpPacket(buffer);

			spec::DhcpHeader dhcp_header;
			memcpy(&dhcp_header, buffer.data(), sizeof(spec::DhcpHeader));

			int dhcp_type;
			walkBootpOptions(buffer, [&] (int tag, const std::byte *opt_data, size_t opt_size) {
				switch(tag) {
				case spec::kDhcpMessageType:
					assert(opt_size == 1);
					dhcp_type = static_cast<int>(*opt_data);
					break;
				case spec::kDhcpServer:
				case spec::kBootpSubnet:
				case spec::kBootpRouters:
				case spec::kBootpDns:
				case spec::kDhcpLeaseTime:
					break;
				default:
					printf("    Unexpected BOOTP option: %d !\n", tag);
				}
			});

			if(dhcp_type != spec::kTypeAck) {
				std::cout << "nic-virtio: Discarding DHCP packet of unexpected type"
						<< std::endl;
				continue;
			}
			co_return true;
		}
	}

	co_return false;
}

static async::result<void> testNetworking(Device *device) {
	while(true) {
		if(!(co_await dhcpDiscover(device)))
			continue;
		if(!(co_await dhcpRequest(device)))
			continue;
		// TODO: Assign the DHCP IP to the network interface.
		break;
	}
}

} } // namespace nic::virtio
