
#include <libnet.hpp>
#include "udp.hpp"

namespace libnet {

struct DhcpDiscover {
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
	packet.resize(sizeof(DhcpDiscover) + 4);

	DhcpDiscover dhcp_discover;
	dhcp_discover.op = 1;
	dhcp_discover.htype = 1;
	dhcp_discover.hlen = 6;
	dhcp_discover.hops = 0;
	dhcp_discover.xid = hostToNet<uint32_t>(3);
	dhcp_discover.secs = hostToNet<uint16_t>(0);
	dhcp_discover.flags = hostToNet<uint16_t>(0x8000);
	dhcp_discover.ciaddr = 0;
	dhcp_discover.yiaddr = 0;
	dhcp_discover.siaddr = 0;
	dhcp_discover.giaddr = 0;
	memset(dhcp_discover.chaddr, 0, 16);
	memcpy(dhcp_discover.chaddr, local_mac.octets, 6);
	memset(dhcp_discover.sname, 0, 64);
	memset(dhcp_discover.file, 0, 128);
	dhcp_discover.magic = hostToNet<uint32_t>(0x63825363);
	memcpy(&packet[0], &dhcp_discover, sizeof(DhcpDiscover));

	auto dhcp_options = &packet[sizeof(DhcpDiscover)];
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

} // namespace libnet

