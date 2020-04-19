#include <nic/virtio/virtio.hpp>

#include <arch/dma_pool.hpp>
#include <core/virtio/core.hpp>

namespace {
// Device feature bits.
constexpr size_t legacyHeaderSize = 10;
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

struct VirtioNic : nic::Link {
	VirtioNic(std::unique_ptr<virtio_core::Transport> transport);

	virtual async::result<void> receive(arch::dma_buffer_view) override;
	virtual async::result<void> send(const arch::dma_buffer_view) override;
	virtual arch::dma_pool *dmaPool() override;

	virtual ~VirtioNic() override = default;
private:
	std::unique_ptr<virtio_core::Transport> m_transport;
	arch::contiguous_pool m_dmaPool;
	virtio_core::Queue *m_receiveVq;
	virtio_core::Queue *m_transmitVq;
};

VirtioNic::VirtioNic(std::unique_ptr<virtio_core::Transport> transport)
	: nic::Link(1500), m_transport { std::move(transport) }
{
	if(m_transport->checkDeviceFeature(VIRTIO_NET_F_MAC)) {
		for (int i = 0; i < 6; i++) {
			mac_[i] = m_transport->loadConfig8(i);
		}
		char ms[3 * 6 + 1];
		sprintf(ms, "%.2x:%.2x:%.2x:%.2x:%.2x:%.2x",
				mac_[0], mac_[1], mac_[2],
				mac_[3], mac_[4], mac_[5]);
		std::cout << "virtio-driver: Device has a hardware MAC: "
			<< ms << std::endl;
		m_transport->acknowledgeDriverFeature(VIRTIO_NET_F_MAC);
	}

	m_transport->finalizeFeatures();
	m_transport->claimQueues(2);
	m_receiveVq = m_transport->setupQueue(0);
	m_transmitVq = m_transport->setupQueue(1);

	m_transport->runDevice();
}

async::result<void> VirtioNic::receive(arch::dma_buffer_view frame) {
	arch::dma_object<VirtHeader> header { &m_dmaPool };

	virtio_core::Chain chain;
	chain.append(co_await m_receiveVq->obtainDescriptor());
	chain.setupBuffer(virtio_core::deviceToHost,
			header.view_buffer().subview(0, legacyHeaderSize));
	chain.append(co_await m_receiveVq->obtainDescriptor());
	chain.setupBuffer(virtio_core::deviceToHost, frame);

	co_await m_receiveVq->submitDescriptor(chain.front());

	co_return;
}

arch::dma_pool *VirtioNic::dmaPool() {
	return &m_dmaPool;
}

async::result<void> VirtioNic::send(const arch::dma_buffer_view payload) {
	if (payload.size() > 1514) {
		throw std::runtime_error("data exceeds mtu");
	}

	arch::dma_object<VirtHeader> header { &m_dmaPool };
	memset(header.data(), 0, sizeof(VirtHeader));

	virtio_core::Chain chain;
	chain.append(co_await m_transmitVq->obtainDescriptor());
	chain.setupBuffer(virtio_core::hostToDevice,
			header.view_buffer().subview(0, legacyHeaderSize));
	chain.append(co_await m_transmitVq->obtainDescriptor());
	chain.setupBuffer(virtio_core::hostToDevice, payload);

	std::cout << "virtio-driver: sending frame" << std::endl;
	co_await m_transmitVq->submitDescriptor(chain.front());
	std::cout << "virtio-driver: sent frame" << std::endl;
}
} // namespace

namespace nic::virtio {

std::shared_ptr<nic::Link> makeShared(
		std::unique_ptr<virtio_core::Transport> transport) {
	return std::make_shared<VirtioNic>(std::move(transport));
}

} // namespace nic::virtio
