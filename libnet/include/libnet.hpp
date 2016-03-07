
#ifndef LIBNET_HPP
#define LIBNET_HPP

#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <string>

namespace libnet {

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

struct NetDevice {
	virtual void sendPacket(std::string packet) = 0;
};

void testDevice(NetDevice &device);

void sendArpRequest();

void onReceive(void *buffer, size_t length);

void receiveArpPacket(void *buffer, size_t length);

void receiveIp4Packet(void *buffer, size_t length);

void receiveUdpPacket(void *buffer, size_t length);

void receiveTcpPacket(void *buffer, size_t length);

void receivePacket(void *buffer, size_t length);

} // namespace libnet

#endif // LIBNET_HPP

