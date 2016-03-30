
#include <stdlib.h>
#include <libnet.hpp>
#include "usernet.hpp"
#include "udp.hpp"
#include "arp.hpp"
#include "dns.hpp"

namespace libnet {

std::string readDnsName(void *packet, uint8_t *&it) {
	std::string name;
	while(true) { // read the record name
		uint8_t code = *(it++);
		if((code & 0xC0) == 0xC0) {
			// this segment is a "pointer"
			uint8_t offset = ((code & 0x3F) << 8) | *(it++);
			auto offset_it = (uint8_t *)packet + offset;
			return name + readDnsName(packet, offset_it);
		}else if(!(code & 0xC0)) {
			// this segment is a length followed by chars
			if(!code)
				return name;

			for(uint8_t i = 0; i < code; i++)
				name += *(it++);
			name += ".";
		}else{
			printf("Illegal octet in DNS name\n");
		}
	}
}

void gotRouterIp(void *object, MacAddress address) {
	printf("Router Mac: %d:%d:%d:%d:%d:%d\n", address.octets[0], address.octets[1],
			address.octets[2], address.octets[3], address.octets[4], address.octets[5]);		
	routerMac = address;
		
	sendDnsRequest();
}

void receivePacket(EthernetInfo link_info, Ip4Info network_info, void *buffer, size_t length) {
	auto dhcp_header = (DhcpHeader *)buffer;
	
	// TODO: verify dhcp packet size

	printf("            BOOTP operation: %d\n", dhcp_header->op);
	auto client_ip = dhcp_header->clientIp;
	auto assigned_ip = dhcp_header->assignedIp;
	auto server_ip = dhcp_header->serverIp;
	auto gateway_ip = dhcp_header->gatewayIp;
	printf("            BOOTP clientIp: %d.%d.%d.%d, assignedIp: %d.%d.%d.%d\n",
			client_ip.octets[0], client_ip.octets[1], client_ip.octets[2], client_ip.octets[3],
			assigned_ip.octets[0], assigned_ip.octets[1], assigned_ip.octets[2], assigned_ip.octets[3]);
	printf("            BOOTP serverIp: %d.%d.%d.%d, gatewayIp: %d.%d.%d.%d\n",
			server_ip.octets[0], server_ip.octets[1], server_ip.octets[2], server_ip.octets[3],
			gateway_ip.octets[0], gateway_ip.octets[1], gateway_ip.octets[2], gateway_ip.octets[3]);

	int dhcp_type;
	Ip4Address dhcp_server;

	size_t offset = 0;
	auto options = (uint8_t *)buffer + sizeof(DhcpHeader);
	while(offset < length - sizeof(DhcpHeader)) {
		uint8_t tag = options[offset];
		if(tag == kBootpNull) {
			continue;
		}else if(tag == kBootpEnd) {
			break;
		}

		uint8_t opt_size = options[offset + 1];
		uint8_t *opt_data = &options[offset + 2];
		if(tag == kBootpSubnet) {
			assert(opt_size == 4);
			subnetMask.octets[0] = opt_data[0];
			subnetMask.octets[1] = opt_data[1];
			subnetMask.octets[2] = opt_data[2];
			subnetMask.octets[3] = opt_data[3];
		}else if(tag == kBootpRouters) {
			assert(opt_size == 4);
			routerIp.octets[0] = opt_data[0];
			routerIp.octets[1] = opt_data[1];
			routerIp.octets[2] = opt_data[2];
			routerIp.octets[3] = opt_data[3];	
		}else if(tag == kBootpDns) {
			assert(opt_size == 4);
			dnsIp.octets[0] = opt_data[0];
			dnsIp.octets[1] = opt_data[1];
			dnsIp.octets[2] = opt_data[2];
			dnsIp.octets[3] = opt_data[3];	
		}else if(tag == kDhcpLeaseTime) {
		
		}else if(tag == kDhcpMessageType) {
			assert(opt_size == 1);
			dhcp_type = *opt_data;
			printf("            DHCP messageType: %d\n", dhcp_type);
		}else if(tag == kDhcpServer) {
			assert(opt_size == 4);
			dhcp_server = Ip4Address(opt_data[0], opt_data[1], opt_data[2], opt_data[3]);
		}else{
			printf("            BOOTP Invalid option: %d !\n", tag);
		}

		offset += 2 + opt_size;
	}

	assert(dhcp_server == network_info.sourceIp);

	if(dhcpState == kDiscoverySent) {
		assert(dhcp_type == kTypeOffer);	
	
		std::string packet;
		packet.resize(sizeof(DhcpHeader) + 16);

		DhcpHeader new_dhcp_header;
		new_dhcp_header.op = 1;
		new_dhcp_header.htype = 1;
		new_dhcp_header.hlen = 6;
		new_dhcp_header.hops = 0;
		new_dhcp_header.transaction = hostToNet<uint32_t>(dhcpTransaction);
		new_dhcp_header.secondsSinceBoot = 0;
		new_dhcp_header.flags = 0;
		new_dhcp_header.clientIp = Ip4Address();
		new_dhcp_header.assignedIp = Ip4Address();
		new_dhcp_header.serverIp = dhcp_header->serverIp;
		new_dhcp_header.gatewayIp = Ip4Address();
		memset(new_dhcp_header.clientHardware, 0, 16);
		memcpy(new_dhcp_header.clientHardware, localMac.octets, 6);
		memset(new_dhcp_header.serverHost, 0, 64);
		memset(new_dhcp_header.file, 0, 128);
		new_dhcp_header.magic = hostToNet<uint32_t>(kDhcpMagic);
		memcpy(&packet[0], &new_dhcp_header, sizeof(DhcpHeader));

		auto dhcp_options = &packet[sizeof(DhcpHeader)];
		dhcp_options[0] = kDhcpMessageType;
		dhcp_options[1] = 1;
		dhcp_options[2] = kTypeRequest;
		dhcp_options[3] = kDhcpServer;
		dhcp_options[4] = 4;
		dhcp_options[5] = dhcp_server.octets[0];
		dhcp_options[6] = dhcp_server.octets[1];
		dhcp_options[7] = dhcp_server.octets[2];
		dhcp_options[8] = dhcp_server.octets[3];
		dhcp_options[9] = kDhcpRequestedIp;
		dhcp_options[10] = 4;
		dhcp_options[11] = assigned_ip.octets[0];
		dhcp_options[12] = assigned_ip.octets[1];
		dhcp_options[13] = assigned_ip.octets[2];
		dhcp_options[14] = assigned_ip.octets[3];
		dhcp_options[15] = kBootpEnd;

		EthernetInfo ethernet_info;
		ethernet_info.sourceMac = localMac;
		ethernet_info.destMac = link_info.sourceMac;
		ethernet_info.etherType = kEtherIp4;

		Ip4Info ip_info;
		ip_info.sourceIp = Ip4Address();
		ip_info.destIp = dhcp_server;
		ip_info.protocol = kUdpProtocol;

		UdpInfo udp_info;
		udp_info.sourcePort = 68;
		udp_info.destPort = 67;

		localIp = assigned_ip;
		dhcpState = kRequestSent;
		printf("kRequestSent!\n");
		sendUdpPacket(*globalDevice, ethernet_info, ip_info, udp_info, packet);
	}else if(dhcpState == kRequestSent) {
		assert(dhcp_type == kTypeAck);
		dhcpState = kAckReceived;

		arpLookup(routerIp, CALLBACK_STATIC(nullptr, &gotRouterIp));
	}else{
		printf("Unexpected DHCP state");
	 	abort();
	}
}

void receiveDnsPacket(void *buffer, size_t length) {
	if(length < sizeof(DnsHeader)) {
		printf("        DNS packet is too short!\n");
		return;
	}
	auto dns_header = (DnsHeader *)buffer;
	if(netToHost<uint16_t>(dns_header->identification) != 123) {
		printf("        DNS identification does not match!\n");
		return;	
	}
	auto dns_flags = netToHost<uint16_t>(dns_header->flags);
	if(!(dns_flags & 0x8000)) {
		printf("        DNS answer is a request!\n");
		return;	
	}
	if(dns_flags & 0x0200) {
		printf("        DNS answer is truncated!\n");
		return;	
	}
	if(dns_flags & 0x0070) {
		printf("        DNS answer has set Z flag!\n");
		return;	
	}
	if((dns_flags & 0x000F) != 0) {
		printf("        Error in DNS RCODE: %d!\n", netToHost<uint16_t>(dns_header->flags) & 0x000F);
		return;		
	}

	auto answers = netToHost<uint16_t>(dns_header->totalAnswerRRs);
	printf("        Count of DNS answers: %d\n", answers);

	auto it = (uint8_t *)buffer + sizeof(DnsHeader);

	// read the DNS questions
	for(int k = 0; k < netToHost<uint16_t>(dns_header->totalQuestions); k++) {
		std::string name = readDnsName(buffer, it);
		printf("QName: %s\n", name.c_str());

		uint16_t qtype = (it[0] << 8) | it[1];
		uint16_t qclass = (it[2] << 8) | it[3];
		it += 4;
	}
	
	// read the DNS answer RRs
	for(int k = 0; k < netToHost<uint16_t>(dns_header->totalAnswerRRs); k++) {
		std::string name = readDnsName(buffer, it);
		printf("Name: %s\n", name.c_str());

		uint16_t rr_type = (it[0] << 8) | it[1];
		uint16_t rr_class = (it[2] << 8) | it[3];
		uint16_t rr_length = (it[8] << 8) | it[9];
		it += 10;

		void *rr_data = it;
		if(rr_type == 1) {
			Ip4Address address;
			memcpy(address.octets, rr_data, sizeof(Ip4Address));
			printf("            A record: %d.%d.%d.%d\n", address.octets[0], address.octets[1], 
					address.octets[2], address.octets[3]);
		}else{
			printf("            Unexpected RR type: %d!\n", rr_type);
		}

		it += rr_length;
	}
}

void sendDnsRequest() {
	std::string packet;
	packet.resize(sizeof(DnsHeader));
	
	DnsHeader dns_header;
	dns_header.identification = hostToNet<uint16_t>(123);
	dns_header.flags = hostToNet<uint16_t>(0x100);
	dns_header.totalQuestions = hostToNet<uint16_t>(1);
	dns_header.totalAnswerRRs = hostToNet<uint16_t>(0);
	dns_header.totalAuthorityRRs = hostToNet<uint16_t>(0);
	dns_header.totalAdditionalRRs = hostToNet<uint16_t>(0);

	memcpy(&packet[0], &dns_header, sizeof(DnsHeader));
	
	uint16_t qtype = 1;
	uint16_t qclass = 1;

	packet += char(3);
	packet += "www";
	packet += char(6);
	packet += "google";
	packet += char(3);
	packet += "com";
	packet += char(0);
	packet += qtype >> 8;
	packet += qtype & 0xFF;
	packet += qclass >> 8;
	packet += qclass & 0xFF;

	EthernetInfo ethernet_info;
	ethernet_info.sourceMac = localMac;
	ethernet_info.destMac = routerMac;
	ethernet_info.etherType = kEtherIp4;

	Ip4Info ip_info;
	ip_info.sourceIp = localIp;
	ip_info.destIp = Ip4Address(8,8,8,8);
	ip_info.protocol = kUdpProtocol;

	UdpInfo udp_info;
	udp_info.sourcePort = 49152;
	udp_info.destPort = 53;
	
	sendUdpPacket(*globalDevice, ethernet_info, ip_info, udp_info, packet);
}

void sendDhcpDiscover(NetDevice &device) {
	std::string packet;
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
	memcpy(dhcp_header.clientHardware, localMac.octets, 6);
	memset(dhcp_header.serverHost, 0, 64);
	memset(dhcp_header.file, 0, 128);
	dhcp_header.magic = hostToNet<uint32_t>(kDhcpMagic);
	memcpy(&packet[0], &dhcp_header, sizeof(DhcpHeader));

	auto dhcp_options = &packet[sizeof(DhcpHeader)];
	dhcp_options[0] = kDhcpMessageType;
	dhcp_options[1] = 1;
	dhcp_options[2] = kTypeDiscover;
	dhcp_options[3] = kBootpEnd;
	
	EthernetInfo ethernet_info;
	ethernet_info.sourceMac = localMac;
	ethernet_info.destMac = MacAddress::broadcast();
	ethernet_info.etherType = kEtherIp4;

	Ip4Info ip_info;
	ip_info.sourceIp = Ip4Address();
	ip_info.destIp = Ip4Address::broadcast();
	ip_info.protocol = kUdpProtocol;

	UdpInfo udp_info;
	udp_info.sourcePort = 68;
	udp_info.destPort = 67;
	
	dhcpState = kDiscoverySent;
	sendUdpPacket(device, ethernet_info, ip_info, udp_info, packet);
}

} // namespace libnet



