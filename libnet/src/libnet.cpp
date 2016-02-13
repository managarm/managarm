
#include <stdio.h>
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

void testDevice(NetDevice &device) {
	MacAddress local_mac;
//	for(size_t i = 0; i < 6; i++)
//		local_mac.octets[i] = readConfig8(i);
	//FIXME

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
	dhcp_options[2] = 1;
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
	} else {
		printf("Invalid ether type!\n");
	}
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
	auto src_ip = Ip4Address(netToHost<uint32_t>(dhcp_header->yiaddr));
	auto dest_ip = Ip4Address(netToHost<uint32_t>(dhcp_header->siaddr));
	printf("Dhcp yiaddr: %d.%d.%d.%d\n", src_ip.octets[0], src_ip.octets[1], src_ip.octets[2], src_ip.octets[3]);
	printf("Dhcp siaddr: %d.%d.%d.%d\n", dest_ip.octets[0], dest_ip.octets[1], dest_ip.octets[2], dest_ip.octets[3]);

	auto options = (uint8_t *)buffer + sizeof(DhcpHeader);
	Ip4Address subnet_mask;
	Ip4Address router;
	Ip4Address dns;

	int offset = 0;
	while(offset < int(length - sizeof(DhcpHeader))) {
		uint8_t tag = options[offset];
		uint8_t opt_size = options[offset + 1];
		uint8_t *opt_data = &options[offset + 2];

		if(tag == kTagNull) {
			// do nothing
		} else if(tag == kTagSubnetMask) {
			assert(opt_size == 4);
			subnet_mask.octets[0] = opt_data[0];
			subnet_mask.octets[1] = opt_data[1];
			subnet_mask.octets[2] = opt_data[2];
			subnet_mask.octets[3] = opt_data[3];
		} else if(tag == kTagRouters) {
			assert(opt_size == 4);
			router.octets[0] = opt_data[0];
			router.octets[1] = opt_data[1];
			router.octets[2] = opt_data[2];
			router.octets[3] = opt_data[3];	
		} else if(tag == kTagDns) {
			assert(opt_size == 4);
			dns.octets[0] = opt_data[0];
			dns.octets[1] = opt_data[1];
			dns.octets[2] = opt_data[2];
			dns.octets[3] = opt_data[3];	
		} else if(tag == kTagIpLeaseTime) {
		
		} else if(tag == kTagDhcpMessageType) {
			assert(opt_size == 1);
			assert(*opt_data == 2);
		} else if(tag == kTagServerIdentifier) {
			
		} else if(tag == kTagEnd) {
			break;
		} else {
			printf("Invalid option: %d !\n", tag);
		}

		offset += 2 + opt_size;
	}
}

} // namespace libnet

