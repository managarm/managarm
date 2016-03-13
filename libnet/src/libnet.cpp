
#include <stdio.h>
#include <stdlib.h>
#include <libnet.hpp>
#include "udp.hpp"
#include "tcp.hpp"
#include "dns.hpp"

namespace libnet {

enum {
	kUdp = 17,
	kTcp = 6,
	kIpVersion4 = 4,
	kIpVersion6 = 6
};

enum {
	kFlagReserved = 0x8000,
	kFlagDF = 0x4000,
	kFlagMF = 0x2000,
	kFragmentOffsetMask = 0x1FFF
};

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

enum {
	// bits of the BOOTP flags field
	kDhcpBroadcast = 0x8000,

	// dhcp magic option
	kDhcpMagic = 0x63825363
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

struct TcpSocket {
	enum State {
		kStateNone,
		kStateSynSent,
		kStateEstablished
	};

	TcpSocket();

	void connect();

	State state;

	// number of the latest byte the remote end has ACKed
	uint32_t ackedLocalSequence;

	// number of byte we expect to receive next
	uint32_t expectedRemoteSequence;

	//std::queue<std::string> resendQueue;
};

TcpSocket::TcpSocket()
: state(kStateNone), ackedLocalSequence(1000) { }

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

uint32_t dhcpTransaction = 0xD61FF088; // some random integer
DhcpState dhcpState = kDefaultState;

NetDevice *globalDevice;
Ip4Address localIp;
MacAddress localMac;
Ip4Address routerIp;
MacAddress routerMac;
Ip4Address dnsIp;
Ip4Address subnetMask;

TcpSocket tcpSocket;

void receiveArpPacket(void *buffer, size_t length);

void receiveIp4Packet(EthernetInfo link_info, void *buffer, size_t length);

void receiveUdpPacket(EthernetInfo link_info, Ip4Info network_info, void *buffer, size_t length);

void receiveTcpPacket(void *buffer, size_t length);

void receivePacket(EthernetInfo link_info, Ip4Info network_info, void *buffer, size_t length);

void TcpSocket::connect() {
	state = kStateSynSent;

	EthernetInfo ethernet_info;
	ethernet_info.sourceMac = localMac;
	ethernet_info.destMac = routerMac;
	ethernet_info.etherType = kEtherIp4;

	Ip4Info ip_info;
	ip_info.sourceIp = localIp;
	ip_info.destIp = Ip4Address(173, 194, 116, 210); //www.google.com
	ip_info.protocol = kTcpProtocol;

	TcpInfo tcp_info;
	tcp_info.srcPort = 49152;
	tcp_info.destPort = 80;
	tcp_info.seqNumber = ackedLocalSequence;
	tcp_info.ackNumber = 0;
	tcp_info.ackFlag = false;
	tcp_info.rstFlag = false;
	tcp_info.synFlag = true;
	tcp_info.finFlag = false;

	std::string packet;
	sendTcpPacket(*globalDevice, ethernet_info, ip_info, tcp_info, packet);
}

void sendArpRequest() {
	std::string packet;
	packet.resize(sizeof(ArpPacket));

	ArpPacket arp_packet;
	arp_packet.hwType = hostToNet<uint16_t>(1);
	arp_packet.protoType = hostToNet<uint16_t>(0x800);
	arp_packet.hwLength = 6;
	arp_packet.protoLength = 4;
	arp_packet.operation = hostToNet<uint16_t>(1);
	arp_packet.senderHw = localMac;
	arp_packet.senderProto = localIp;
	arp_packet.targetHw = MacAddress::broadcast();
	arp_packet.targetProto = routerIp;

	memcpy(&packet[0], &arp_packet, sizeof(ArpPacket));
	
	EthernetInfo ethernet_info;
	ethernet_info.sourceMac = localMac;
	ethernet_info.destMac = MacAddress::broadcast();
	ethernet_info.etherType = kEtherArp;

	sendEthernetPacket(*globalDevice, ethernet_info, packet);
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


void testDevice(NetDevice &device, uint8_t mac_octets[6]) {
	globalDevice = &device;
	memcpy(localMac.octets, mac_octets, 6);

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

void onReceive(void *buffer, size_t length) {
	if(length < sizeof(EthernetHeader)) {
		printf("Ethernet: Packet is too short!\n");
		return;
	}

	auto ethernet_header = (EthernetHeader *)buffer;

	EthernetInfo link_info;
	link_info.sourceMac = ethernet_header->sourceAddress;
	link_info.destMac = ethernet_header->destAddress;
	link_info.etherType = netToHost<uint16_t>(ethernet_header->etherType);
	
	void *payload_buffer = (char *)buffer + sizeof(EthernetHeader);
	size_t payload_length = length - sizeof(EthernetHeader);

	if(link_info.destMac != localMac && link_info.destMac != MacAddress::broadcast()) {
//		printf("Destination mismatch\n");
		return;
	}
	
	printf("Ethernet frame. srcMac: %x:%x:%x:%x:%x:%x, destMac: %x:%x:%x:%x:%x:%x, etherType: 0x%x\n",
			link_info.sourceMac.octets[0], link_info.sourceMac.octets[1],
			link_info.sourceMac.octets[2], link_info.sourceMac.octets[3],
			link_info.sourceMac.octets[4], link_info.sourceMac.octets[5],
			link_info.destMac.octets[0], link_info.destMac.octets[1],
			link_info.destMac.octets[2], link_info.destMac.octets[3],
			link_info.destMac.octets[4], link_info.destMac.octets[5],
			link_info.etherType);
	
	if(link_info.etherType == kEtherIp4) {
		receiveIp4Packet(link_info, payload_buffer, payload_length);
	} else if(link_info.etherType == kEtherArp) {
		receiveArpPacket(payload_buffer, payload_length);
	} else {
		printf("    Ethernet: Unexpected etherType 0x%X!\n", link_info.etherType);
	}
}

void receiveArpPacket(void *buffer, size_t length) {
	printf("sizeof(ArpPacket): %lu\n", length);
	if(length != sizeof(ArpPacket)) {
		printf("Length is not the actual ArpPacket length!\n");
// 		return;
	}
	
	auto arp_packet = (ArpPacket *)buffer;
	localMac = arp_packet->targetHw;
	printf("localMac: %d:%d:%d:%d:%d:%d\n", localMac.octets[0], localMac.octets[1],
			localMac.octets[2], localMac.octets[3], localMac.octets[4], localMac.octets[5]);		
	routerMac = arp_packet->senderHw;
	printf("routerMac: %d:%d:%d:%d:%d:%d\n", routerMac.octets[0], routerMac.octets[1],
			routerMac.octets[2], routerMac.octets[3], routerMac.octets[4], routerMac.octets[5]);
	
	printf("hwtype: %i\n", netToHost<uint16_t>(arp_packet->hwType));
	printf("prototype: %i\n", netToHost<uint16_t>(arp_packet->protoType));
	printf("hwlength: %i\n", arp_packet->hwLength);
	printf("protolength: %i\n", arp_packet->protoLength);
	printf("operation: %i\n", netToHost<uint16_t>(arp_packet->operation));

	sendDnsRequest();	
	//tcpSocket.connect();
}

void receiveIp4Packet(EthernetInfo link_info, void *buffer, size_t length) {
	if(length < sizeof(Ip4Header)) {
		printf("    Ip4: Packet is too short!\n");
		return;
	}
	
	auto ip_header = (Ip4Header *)buffer;

	if((ip_header->version_headerLength >> 4) != kIpVersion4) {
		printf("    Ip4: Version not supported!\n");
		return;
	}

	size_t header_length = (ip_header->version_headerLength & 0x0F) * 4;
	if(header_length < sizeof(Ip4Header)) {
		printf("    Ip4: headerLength is too small!\n");
		return;
	}else if(netToHost<uint16_t>(ip_header->length) < header_length) {
		printf("    Ip4: totalLength < headerLength!\n");
		return;
	}else if(netToHost<uint16_t>(ip_header->length) != length) {
		printf("    Ip4: totalLength does not match packet length!\n");
		return;
	}

	Ip4Info network_info;
	network_info.sourceIp = ip_header->sourceIp;
	network_info.destIp = ip_header->targetIp;
	network_info.protocol = ip_header->protocol;
	
	void *payload_buffer = (char *)buffer + header_length;
	size_t payload_length = length - header_length;
	
	printf("    Ip4 header. srcIp: %d.%d.%d.%d, destIp: %d.%d.%d.%d, protocol: %d\n",
			network_info.sourceIp.octets[0], network_info.sourceIp.octets[1],
			network_info.sourceIp.octets[2], network_info.sourceIp.octets[3],
			network_info.destIp.octets[0], network_info.destIp.octets[1],
			network_info.destIp.octets[2], network_info.destIp.octets[3],
			network_info.protocol);
	printf("    headerLength: %lu, payloadLength: %lu\n", header_length, payload_length);
	
	auto flags = netToHost<uint16_t>(ip_header->flags_offset);
	auto fragment_offset = flags & kFragmentOffsetMask;
	
	assert(!(flags & kFlagReserved));
	if(fragment_offset != 0) {
		printf("    Ip4: Non-zero fragment offset %d not implemented!\n", fragment_offset);
		return;
	}else if(flags & kFlagMF) {
		printf("    Ip4: MF flag not implemented\n");
		return;
	}

	printf("    flags:");
	if(flags & kFlagDF)
		printf(" DF");
	if(flags & kFlagMF)
		printf(" MF");
	printf("\n");

	if(network_info.protocol == kUdp) {
		receiveUdpPacket(link_info, network_info, payload_buffer, payload_length);
	} else if(network_info.protocol == kTcp) {
		receiveTcpPacket(payload_buffer, payload_length);
	} else {
		printf("    Invalid Ip4 protocol type!\n");
	}
}

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

void receiveUdpPacket(EthernetInfo link_info, Ip4Info network_info, void *buffer, size_t length) {
	if(length < sizeof(UdpHeader)) {
		printf("Udp packet is too short!\n");
		return;
	}

	auto udp_header = (UdpHeader *)buffer;
	
	printf("        UDP header. srcPort: %d, destPort: %d\n", netToHost<uint16_t>(udp_header->source),
			netToHost<uint16_t>(udp_header->destination));

	if(netToHost<uint16_t>(udp_header->length) != length) {
		printf("        UDP: Invalid length!\n");
		return;
	}

	void *payload_buffer = (char *)buffer + sizeof(UdpHeader);
	size_t payload_length = length - sizeof(UdpHeader);

	if(netToHost<uint16_t>(udp_header->source) == 67
			&& netToHost<uint16_t>(udp_header->destination) == 68)
		receivePacket(link_info, network_info, payload_buffer, payload_length);
	if(netToHost<uint16_t>(udp_header->source) == 53)
		receiveDnsPacket(payload_buffer, payload_length);
}


void receiveTcpPacket(void *buffer, size_t length) {
	if(length < sizeof(TcpHeader)) {
		printf("        Tcp packet is too short!\n");
		return;
	}

	auto tcp_header = (TcpHeader *)buffer;

	printf("        srcPort: %d\n", netToHost<uint16_t>(tcp_header->srcPort));
	printf("        destPort: %d\n", netToHost<uint16_t>(tcp_header->destPort));
	printf("        seqNumber: %d\n", netToHost<uint32_t>(tcp_header->seqNumber));
	printf("        ackNumber: %d\n", netToHost<uint32_t>(tcp_header->ackNumber));

	printf("        flags:");
	uint16_t flags = netToHost<uint16_t>(tcp_header->flags);
	if(flags & TcpFlags::kTcpFin)
		printf(" FIN");
	if(flags & TcpFlags::kTcpSyn)
		printf(" SYN");
	if(flags & TcpFlags::kTcpAck)
		printf(" ACK");
	if(flags & TcpFlags::kTcpRst)
		printf(" RST");
	printf("\n");

	printf("        dataOffset: %d\n", flags >> 12);

	void *payload_buffer = (char *)buffer + (flags >> 12) * 4;
	size_t payload_length = length - (flags >> 12) * 4;
		
	if(flags & TcpFlags::kTcpRst) {
		printf("        TCP socket is reset\n");
		return;
	}

	if(tcpSocket.state == TcpSocket::kStateSynSent) {
		if(!(flags & TcpFlags::kTcpSyn) || !(flags & TcpFlags::kTcpAck)) {
			printf("        Expected SYN-ACK in SYN-SENT state\n");
			return;
		}else if(flags & TcpFlags::kTcpFin) {
			printf("        FIN set in SYN-SENT state\n");
			return;
		}

		if(payload_length != 0) {
			printf("        SYN-ACK carries payload!\n");
			return;
		}

		if(netToHost<uint32_t>(tcp_header->ackNumber) != tcpSocket.ackedLocalSequence + 1) {
			printf("        Bad ackNumber in SYN-ACK packet!\n");
			return;
		}
		tcpSocket.ackedLocalSequence = netToHost<uint32_t>(tcp_header->ackNumber);
		tcpSocket.expectedRemoteSequence = netToHost<uint32_t>(tcp_header->seqNumber) + 1;
		tcpSocket.state = TcpSocket::kStateEstablished;

		EthernetInfo ethernet_info;
		ethernet_info.sourceMac = localMac;
		ethernet_info.destMac = routerMac;
		ethernet_info.etherType = kEtherIp4;

		Ip4Info ip_info;
		ip_info.sourceIp = localIp;
		ip_info.destIp = Ip4Address(173, 194, 116, 210); //www.google.com
		ip_info.protocol = kTcpProtocol;

		TcpInfo tcp_info;
		tcp_info.srcPort = 49152;
		tcp_info.destPort = 80;
		tcp_info.seqNumber = tcpSocket.ackedLocalSequence;
		tcp_info.ackNumber = tcpSocket.expectedRemoteSequence;
		tcp_info.ackFlag = true;
		tcp_info.rstFlag = false;
		tcp_info.synFlag = false;
		tcp_info.finFlag = false;

		std::string packet("GET /\n");
		sendTcpPacket(*globalDevice, ethernet_info, ip_info, tcp_info, packet);
	}else if(tcpSocket.state == TcpSocket::kStateEstablished) {
		if(flags & TcpFlags::kTcpSyn) {
			printf("        SYN set in ESTABLISHED state\n");
			return;
		}

		if(netToHost<uint32_t>(tcp_header->seqNumber) != tcpSocket.expectedRemoteSequence) {
			printf("        Packet out-of-order!\n");
			return;
		}
		
		if(flags & TcpFlags::kTcpAck) {
			tcpSocket.ackedLocalSequence = netToHost<uint32_t>(tcp_header->ackNumber);
		}

		size_t virtual_length = payload_length;
		if(flags & TcpFlags::kTcpFin)
			virtual_length++;

		tcpSocket.expectedRemoteSequence = netToHost<uint32_t>(tcp_header->seqNumber) + virtual_length;
		printf("        expectedRemoteSequence: %d\n", tcpSocket.expectedRemoteSequence);

		if(virtual_length > 0) {
			EthernetInfo ethernet_info;
			ethernet_info.sourceMac = localMac;
			ethernet_info.destMac = routerMac;
			ethernet_info.etherType = kEtherIp4;

			Ip4Info ip_info;
			ip_info.sourceIp = localIp;
			ip_info.destIp = Ip4Address(173, 194, 116, 210); //www.google.com
			ip_info.protocol = kTcpProtocol;

			TcpInfo tcp_info;
			tcp_info.srcPort = 49152;
			tcp_info.destPort = 80;
			tcp_info.seqNumber = tcpSocket.ackedLocalSequence;
			tcp_info.ackNumber = tcpSocket.expectedRemoteSequence;
			tcp_info.ackFlag = true;
			tcp_info.rstFlag = false;
			tcp_info.synFlag = false;
			tcp_info.finFlag = false;

			sendTcpPacket(*globalDevice, ethernet_info, ip_info, tcp_info, std::string());
		}
	}else{
		printf("        TCP socket in illegal state\n");
		return;
	}

	fwrite(payload_buffer, 1, payload_length, stdout);
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
		sendArpRequest();
	}else{
		printf("Unexpected DHCP state");
	 	abort();
	}
}

} // namespace libnet

