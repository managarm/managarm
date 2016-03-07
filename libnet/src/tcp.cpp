
#include <stdio.h>
#include <libnet.hpp>
#include "tcp.hpp"

namespace libnet {

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

} // namespace libnet

