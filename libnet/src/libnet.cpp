
#include <stdio.h>
#include <stdlib.h>
#include <libnet.hpp>
#include "udp.hpp"

namespace libnet {

enum {
	kUdp = 17,
	kIpVersion4 = 4,
	kIpVersion6 = 6
};

enum {
	kFragmentReserved = 0x8000,
	kFragmentDF = 0x4000,
	kFragmentMF = 0x2000,
	kFragmentOffsetMask = 0x1FFF
};

enum {
	kTagNull = 0,
	kTagSubnetMask = 1,
	kTagRouters = 3,
	kTagDns = 6,
	kTagIpLeaseTime = 51,
	kTagDhcpMessageType = 53,
	kTagServerIdentifier = 54,
	kTagEnd = 255
};

struct DhcpHeader {
	uint8_t op;
	uint8_t htype;
	uint8_t hlen;
	uint8_t hops;
	uint32_t xid;
	uint16_t secs;
	uint16_t flags;
	uint32_t ciaddr;
	uint32_t yiaddr;
	uint32_t siaddr;
	uint32_t giaddr;
	uint8_t chaddr[16];
	uint8_t sname[64];
	uint8_t file[128];
	uint32_t magic;
};

struct ArpPacket {
	uint16_t hwType;
	uint16_t protoType;
	uint8_t hwLength, protoLength;
	uint16_t operation;
	MacAddress senderHw;
	Ip4Address senderProto;
	MacAddress targetHw;
	Ip4Address targetProto;
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

enum DhcpState {
	kDefaultState,
	kDiscoverySent,
	kRequestSent,
	kAckReceived
};

DhcpState dhcpState = kDefaultState;
NetDevice *globalDevice;
Ip4Address localIp;
MacAddress localAddress;
Ip4Address routerIp;
MacAddress routerAddress;
Ip4Address dns;
Ip4Address subnetMask;


void sendArpRequest() {
	std::string packet;
	packet.resize(sizeof(ArpPacket));

	ArpPacket arp_packet;
	arp_packet.hwType = hostToNet<uint16_t>(1);
	arp_packet.protoType = hostToNet<uint16_t>(0x800);
	arp_packet.hwLength = 6;
	arp_packet.protoLength = 4;
	arp_packet.operation = hostToNet<uint16_t>(1);
	MacAddress local_mac;
	arp_packet.senderHw = local_mac;
	arp_packet.senderProto = localIp;
	arp_packet.targetHw = MacAddress::broadcast();
	arp_packet.targetProto = routerIp;

	memcpy(&packet[0], &arp_packet, sizeof(ArpPacket));
	
	EthernetInfo ethernet_info;
	ethernet_info.sourceMac = local_mac;
	ethernet_info.destMac = MacAddress::broadcast();
	ethernet_info.etherType = kEtherArp;

	sendEthernetPacket(*globalDevice, ethernet_info, packet);
}


void testDevice(NetDevice &device) {
	MacAddress local_mac;
//	for(size_t i = 0; i < 6; i++)
//		local_mac.octets[i] = readConfig8(i);
	//FIXME
	
	globalDevice = &device;

	std::string packet;
	packet.resize(sizeof(DhcpHeader) + 4);

	DhcpHeader dhcp_header;
	dhcp_header.op = 1;
	dhcp_header.htype = 1;
	dhcp_header.hlen = 6;
	dhcp_header.hops = 0;
	dhcp_header.xid = hostToNet<uint32_t>(3);
	dhcp_header.secs = hostToNet<uint16_t>(0);
	dhcp_header.flags = hostToNet<uint16_t>(0x8000);
	dhcp_header.ciaddr = 0;
	dhcp_header.yiaddr = 0;
	dhcp_header.siaddr = 0;
	dhcp_header.giaddr = 0;
	memset(dhcp_header.chaddr, 0, 16);
	memcpy(dhcp_header.chaddr, local_mac.octets, 6);
	memset(dhcp_header.sname, 0, 64);
	memset(dhcp_header.file, 0, 128);
	dhcp_header.magic = hostToNet<uint32_t>(0x63825363);
	memcpy(&packet[0], &dhcp_header, sizeof(DhcpHeader));

	auto dhcp_options = &packet[sizeof(DhcpHeader)];
	dhcp_options[0] = 53;
	dhcp_options[1] = 1;
	dhcp_options[2] = kTypeDiscover;
	dhcp_options[3] = 0xFF;
	
	EthernetInfo ethernet_info;
	ethernet_info.sourceMac = local_mac;
	ethernet_info.destMac = MacAddress::broadcast();
	ethernet_info.etherType = kEtherIp4;

	Ip4Info ip_info;
	ip_info.sourceIp = Ip4Address(0, 0, 0, 0);
	ip_info.destIp = Ip4Address(0xFF, 0xFF, 0xFF, 0xFF);
	ip_info.protocol = kUdpProtocol;

	UdpInfo udp_info;
	udp_info.sourcePort = 68;
	udp_info.destPort = 67;
	
	dhcpState = kDiscoverySent;
	sendUdpPacket(device, ethernet_info, ip_info, udp_info, packet);
}

void onReceive(void *buffer, size_t length) {
	if(length < sizeof(EthernetHeader)) {
		printf("Ethernet packet is too short!\n");
		return;
	}

	auto ethernet_header = (EthernetHeader *)buffer;

	printf("Sender MAC: %x:%x:%x:%x:%x:%x\n", ethernet_header->sourceAddress[0], ethernet_header->sourceAddress[1],
			ethernet_header->sourceAddress[2], ethernet_header->sourceAddress[3],
			ethernet_header->sourceAddress[4], ethernet_header->sourceAddress[5]);
	printf("Destination MAC: %x:%x:%x:%x:%x:%x\n", ethernet_header->destAddress[0], ethernet_header->destAddress[1],
			ethernet_header->destAddress[2], ethernet_header->destAddress[3],
			ethernet_header->destAddress[4], ethernet_header->destAddress[5]);
	printf("Ethertype: %d\n", netToHost<uint16_t>(ethernet_header->etherType));
	
	if(netToHost<uint16_t>(ethernet_header->etherType) == kEtherIp4) {
		void *ip4_buffer = (char *)buffer + sizeof(EthernetHeader);
		size_t buffer_length = length - sizeof(EthernetHeader);
		receiveIp4Packet(ip4_buffer, buffer_length);
	} else if(netToHost<uint16_t>(ethernet_header->etherType) == kEtherArp) {
		void *arp_buffer = (char *)buffer + sizeof(EthernetHeader);
		size_t buffer_length = length - sizeof(EthernetHeader);
		receiveArpPacket(arp_buffer, buffer_length);
	} else {
		printf("Invalid ether type!\n");
	}
}

void receiveArpPacket(void *buffer, size_t length) {
	if(length != sizeof(ArpPacket)) {
		printf("ArpPacket is too short!\n");
		return;
	}
	
	auto arp_packet = (ArpPacket *)buffer;
	routerAddress = arp_packet->senderHw;
	printf("routerAddress: %d:%d:%d:%d:%d:%d\n", routerAddress.octets[0], routerAddress.octets[1],
	routerAddress.octets[2], routerAddress.octets[3], routerAddress.octets[4], routerAddress.octets[5]);
}

void receiveIp4Packet(void *buffer, size_t length) {
	if(length < sizeof(Ip4Header)) {
		printf("Ip packet is too short!\n");
		return;
	}
	
	auto ip_header = (Ip4Header *)buffer;

	assert((ip_header->flags_offset & kFragmentReserved) == 0);
	if((ip_header->version_headerLength >> 4) != kIpVersion4) {
		printf("Ip version not supported!\n");
		return;
	}

	auto header_length = (ip_header->version_headerLength & 0x0F) * 4;
	printf("Ip4 Length: %d\n", header_length);
		
	if(header_length < (int)sizeof(Ip4Header)) {
		printf("Invalid Ip4->IHL!\n");
		return;
	}
	
	if((int)length < header_length) {
		printf("Ip4 packet is too short!\n");
		return;
	}

	if((ip_header->flags_offset & kFragmentOffsetMask) != 0) {
		printf("Invalid Ip4 offset!\n");
		return;
	}

	if(netToHost<uint16_t>(ip_header->length) < header_length) {
		printf("Invalid Ip4 length!\n");
		return;
	}

	if(ip_header->flags_offset & kFragmentMF) {
		printf("More Fragments not implemented!\n");	
	} else {
		if(ip_header->protocol == kUdp) {
			void *udp_buffer = (char *)buffer + header_length;
			size_t buffer_length = length - header_length;
			receiveUdpPacket(udp_buffer, buffer_length);
		} else {
			printf("Invalid Ip4 protocol type!\n");
		}
	}
}

void receiveUdpPacket(void *buffer, size_t length) {
	if(length < sizeof(UdpHeader)) {
		printf("Udp packet is too short!\n");
		return;
	}

	auto udp_header = (UdpHeader *)buffer;
	
	printf("SrcPort: %d\n", netToHost<uint16_t>(udp_header->source));
	printf("DestPort: %d\n", netToHost<uint16_t>(udp_header->destination));

	if(netToHost<uint16_t>(udp_header->length) != length) {
		printf("udp_header->length: %d , length: %lu\n", netToHost<uint16_t>(udp_header->length), length);
		printf("Invalid Udp length!\n");
		return;
	}

	void *packet_buffer = (char *)buffer + sizeof(UdpHeader);
	size_t buffer_length = length - sizeof(UdpHeader);
	receivePacket(packet_buffer, buffer_length);
}

void receivePacket(void *buffer, size_t length) {
	auto dhcp_header = (DhcpHeader *)buffer;
	
	printf("Dhcp operation: %d\n", dhcp_header->op);
	localIp = Ip4Address(netToHost<uint32_t>(dhcp_header->yiaddr));
	auto si_addr = Ip4Address(netToHost<uint32_t>(dhcp_header->siaddr));
	printf("Dhcp yiaddr: %d.%d.%d.%d\n", localIp.octets[0], localIp.octets[1], localIp.octets[2], localIp.octets[3]);
	printf("Dhcp siaddr: %d.%d.%d.%d\n", si_addr.octets[0], si_addr.octets[1], si_addr.octets[2], si_addr.octets[3]);


	auto options = (uint8_t *)buffer + sizeof(DhcpHeader);
	int dhcp_type;

	int offset = 0;
	while(offset < int(length - sizeof(DhcpHeader))) {
		uint8_t tag = options[offset];
		uint8_t opt_size = options[offset + 1];
		uint8_t *opt_data = &options[offset + 2];

		if(tag == kTagNull) {
			// do nothing
		} else if(tag == kTagSubnetMask) {
			assert(opt_size == 4);
			subnetMask.octets[0] = opt_data[0];
			subnetMask.octets[1] = opt_data[1];
			subnetMask.octets[2] = opt_data[2];
			subnetMask.octets[3] = opt_data[3];
		} else if(tag == kTagRouters) {
			assert(opt_size == 4);
			routerIp.octets[0] = opt_data[0];
			routerIp.octets[1] = opt_data[1];
			routerIp.octets[2] = opt_data[2];
			routerIp.octets[3] = opt_data[3];	
		} else if(tag == kTagDns) {
			assert(opt_size == 4);
			dns.octets[0] = opt_data[0];
			dns.octets[1] = opt_data[1];
			dns.octets[2] = opt_data[2];
			dns.octets[3] = opt_data[3];	
		} else if(tag == kTagIpLeaseTime) {
		
		} else if(tag == kTagDhcpMessageType) {
			assert(opt_size == 1);
			dhcp_type = *opt_data;
			printf("dhcp_type: %d\n", dhcp_type);
		} else if(tag == kTagServerIdentifier) {
			
		} else if(tag == kTagEnd) {
			break;
		} else {
			printf("Invalid option: %d !\n", tag);
		}

		offset += 2 + opt_size;
	}
	
	if(dhcpState == kDiscoverySent) {
		assert(dhcp_type == kTypeOffer);	
	
		MacAddress local_mac;

		std::string packet;
		packet.resize(sizeof(DhcpHeader) + 4);

		DhcpHeader new_dhcp_header;
		new_dhcp_header.op = 1;
		new_dhcp_header.htype = 1;
		new_dhcp_header.hlen = 6;
		new_dhcp_header.hops = 0;
		new_dhcp_header.xid = hostToNet<uint32_t>(3);
		new_dhcp_header.secs = hostToNet<uint16_t>(0);
		new_dhcp_header.flags = hostToNet<uint16_t>(0x0000);
		new_dhcp_header.ciaddr = 0;
		new_dhcp_header.yiaddr = 0;
		new_dhcp_header.siaddr = hostToNet<uint32_t>((si_addr.octets[0] >> 24) + (si_addr.octets[1] >> 16) 
				+ (si_addr.octets[2] >> 8) + (si_addr.octets[3]));
		new_dhcp_header.giaddr = 0;
		memset(new_dhcp_header.chaddr, 0, 16);
		memcpy(new_dhcp_header.chaddr, local_mac.octets, 6);
		memset(new_dhcp_header.sname, 0, 64);
		memset(new_dhcp_header.file, 0, 128);
		new_dhcp_header.magic = hostToNet<uint32_t>(0x63825363);
		memcpy(&packet[0], &new_dhcp_header, sizeof(DhcpHeader));

		auto dhcp_options = &packet[sizeof(DhcpHeader)];
		dhcp_options[0] = 53;
		dhcp_options[1] = 1;
		dhcp_options[2] = kTypeRequest;
		dhcp_options[3] = 0xFF;

		EthernetInfo ethernet_info;
		ethernet_info.sourceMac = local_mac;
		ethernet_info.destMac = MacAddress::broadcast();
		ethernet_info.etherType = kEtherIp4;

		Ip4Info ip_info;
		ip_info.sourceIp = Ip4Address(0, 0, 0, 0);
		ip_info.destIp = Ip4Address(0xFF, 0xFF, 0xFF, 0xFF);
		ip_info.protocol = kUdpProtocol;

		UdpInfo udp_info;
		udp_info.sourcePort = 68;
		udp_info.destPort = 67;

		dhcpState = kRequestSent;
		printf("kRequestSent!\n");
		sendUdpPacket(*globalDevice, ethernet_info, ip_info, udp_info, packet);
	}else if(dhcpState == kRequestSent) {
		assert(dhcp_type == kTypeAck);
		dhcpState = kAckReceived;
		sendArpRequest();
	}else{
		printf("Unexpected DHCP state");
	 	abort();
	}
}

} // namespace libnet

