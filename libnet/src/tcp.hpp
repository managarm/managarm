
#ifndef LIBNET_TCP_HPP
#define LIBNET_TCP_HPP

#include <string>
#include "ip4.hpp"

namespace libnet {

struct TcpInfo {
	uint16_t srcPort;
	uint16_t destPort;
	uint32_t seqNumber;
	uint32_t ackNumber;
	bool ackFlag, rstFlag, synFlag, finFlag;
};

enum TcpFlags {
	kTcpFin = 1,
	kTcpSyn = 2,
	kTcpRst = 4,
	kTcpAck = 16
};

struct TcpHeader {
	uint16_t srcPort;
	uint16_t destPort;
	uint32_t seqNumber;
	uint32_t ackNumber;
	uint16_t flags;
	uint16_t window;
	uint16_t checksum;
	uint16_t urgentPointer;
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

extern TcpSocket tcpSocket;

void sendTcpPacket(NetDevice &device, EthernetInfo link_info, Ip4Info network_info, 
		TcpInfo transport_info, std::string payload);

void receiveTcpPacket(void *buffer, size_t length);

} // namespace libnet

#endif // LIBNET_TCP_HPP

