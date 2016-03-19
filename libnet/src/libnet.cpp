
#include <stdio.h>
#include <stdlib.h>
#include <libnet.hpp>
#include "udp.hpp"
#include "tcp.hpp"
#include "dns.hpp"
#include "arp.hpp"
#include "usernet.hpp"
#include "ethernet.hpp"

namespace libnet {

uint32_t dhcpTransaction = 0xD61FF088; // some random integer
DhcpState dhcpState = kDefaultState;

NetDevice *globalDevice;
Ip4Address dnsIp;
Ip4Address localIp;
Ip4Address routerIp;
Ip4Address subnetMask;
MacAddress localMac;
MacAddress routerMac;
TcpSocket tcpSocket;

void receivePacket(EthernetInfo link_info, Ip4Info network_info, void *buffer, size_t length);

void testDevice(NetDevice &device, uint8_t mac_octets[6]) {
	globalDevice = &device;
	memcpy(localMac.octets, mac_octets, 6);
	
	sendDhcpDiscover(device);
}

void onReceive(void *buffer, size_t length) {
	receiveEthernetPacket(buffer, length);
}

} // namespace libnet

