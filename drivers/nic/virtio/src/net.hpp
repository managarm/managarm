#pragma once

#include <queue>

#include <arch/dma_pool.hpp>
#include <core/virtio/core.hpp>

namespace nic {
namespace virtio {

// --------------------------------------------------------
// VirtIO data structures and constants
// --------------------------------------------------------

// Device feature bits.
enum {
	VIRTIO_NET_F_MAC = 5
};

// Bits for VirtHeader::flags.
enum {
	VIRTIO_NET_HDR_F_NEEDS_CSUM = 1
};

// Values for VirtHeader::gsoType.
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
	uint16_t numBuffers;
};

static constexpr size_t legacyHeaderSize = 10;
static constexpr size_t multiBuffersHeaderSize = 12;
static_assert(sizeof(VirtHeader) == multiBuffersHeaderSize);

// --------------------------------------------------------
// Device
// --------------------------------------------------------

struct Device {
	Device(std::unique_ptr<virtio_core::Transport> transport);

	void runDevice();

	async::result<void> sendPacket(const std::vector<std::byte> &payload);

private:
	async::detached _processReceive();
	// Submits requests from _pendingQueue to the device.
	async::detached _processTransmit();

	std::unique_ptr<virtio_core::Transport> _transport;
	arch::contiguous_pool _dmaPool;

	// The receive/transmit queues of this device.
	virtio_core::Queue *_receiveVq;
	virtio_core::Queue *_transmitVq;
};

} } // namespace nic::virtio
