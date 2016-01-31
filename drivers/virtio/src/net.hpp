
#include <queue>
#include <string>
#include <helx.hpp>
#include <libnet.hpp>

#include "virtio.hpp"

namespace virtio {
namespace net {

// --------------------------------------------------------
// VirtHeader 
// --------------------------------------------------------

enum {
	VIRTIO_NET_HDR_F_NEEDS_CSUM = 1
};

enum {
	VIRTIO_NET_HDR_GSO_NONE = 0,
	VIRTIO_NET_HDR_GSO_TCPV4 = 1,
	VIRTIO_NET_HDR_GSO_UDP = 2,
	VIRTIO_NET_HDR_GSO_TCPV6 = 3,
	VIRTIO_NET_HDR_GSO_ECN = 0x80
};

struct VirtHeader {
	uint8_t flags;
	uint8_t gsoType;
	uint16_t hdrLen;
	uint16_t gsoSize;
	uint16_t csumStart;
	uint16_t csumOffset;
};

// --------------------------------------------------------
// NetDevice
// --------------------------------------------------------

struct Device : public GenericDevice, public libnet::NetDevice {
	Device();

	void doInitialize() override;
	void retrieveDescriptor(size_t queue_index, size_t desc_index) override;
	void afterRetrieve() override;

	void sendPacket(std::string packet) override;

	void testDevice();
	void onInterrupt(HelError error);

private:
	Queue receiveQueue;
	Queue transmitQueue;

	helx::Irq irq;
};

} } // namespace virtio::net

