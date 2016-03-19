
#include <stdio.h>
#include <libnet.hpp>
#include "tcp.hpp"

namespace libnet {

TcpSocket::TcpSocket() {
	state = kStateNone;
	ackedLocalSequence = 1000;
}

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

void sendTcpPacket(NetDevice &device, EthernetInfo link_info, Ip4Info network_info, 
		TcpInfo transport_info, std::string payload) {
	TcpHeader header;
	header.srcPort = hostToNet<uint16_t>(transport_info.srcPort);
	header.destPort = hostToNet<uint16_t>(transport_info.destPort);
	header.seqNumber = hostToNet<uint32_t>(transport_info.seqNumber);
	header.ackNumber = hostToNet<uint32_t>(transport_info.ackNumber);

	uint16_t flags = (sizeof(TcpHeader) / 4) << 12;
	if(transport_info.finFlag)
		flags |= TcpFlags::kTcpFin;
	if(transport_info.synFlag)
		flags |= TcpFlags::kTcpSyn;
	if(transport_info.rstFlag)
		flags |= TcpFlags::kTcpRst;
	if(transport_info.ackFlag)
		flags |= TcpFlags::kTcpAck;
	header.flags = hostToNet<uint16_t>(flags);

	header.window = 0xFFFF;
	header.checksum = 0;
	header.urgentPointer = 0;

	// calculate the TCP checksum
	PseudoIp4Header pseudo;
	memcpy(pseudo.sourceIp, network_info.sourceIp.octets, 4);
	memcpy(pseudo.destIp, network_info.destIp.octets, 4);
	pseudo.reserved = 0;
	pseudo.protocol = kTcpProtocol;
	pseudo.length = hostToNet<uint16_t>(sizeof(TcpHeader) + payload.length());

	Checksum tcp_checksum;
	tcp_checksum.update(&pseudo, sizeof(PseudoIp4Header));
	tcp_checksum.update(&header, sizeof(TcpHeader));
	tcp_checksum.update(payload.data(), payload.size());
	header.checksum = hostToNet<uint16_t>(tcp_checksum.finish());

	std::string packet(sizeof(TcpHeader) + payload.length(), 0);
	memcpy(&packet[0], &header, sizeof(TcpHeader));
	memcpy(&packet[sizeof(TcpHeader)], payload.data(), payload.length());
	
	sendIp4Packet(device, link_info, network_info, packet);
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

} // namespace libnet

