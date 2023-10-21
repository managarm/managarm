#include <nic/virtio/virtio.hpp>

#include <algorithm>

#include <arch/dma_pool.hpp>
#include <async/result.hpp>
#include <bragi/helpers-std.hpp>
#include <hel.h>
#include <hel-syscalls.h>
#include <core/virtio/core.hpp>
#include <core/nic/core.hpp>
#include <helix/ipc.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/svrctl/server.hpp>

#include "net.bragi.hpp"

namespace {
	constexpr bool logFrames = false;
}

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

// TODO: make this inherit some generic nic again
struct VirtioNic : nic_core::Nic {
	VirtioNic(std::unique_ptr<virtio_core::Transport> transport);

	async::result<size_t> receive(arch::dma_buffer_view) override;
	async::result<void> send(const arch::dma_buffer_view) override;

	~VirtioNic() = default;
private:
	std::unique_ptr<virtio_core::Transport> transport_;
	arch::contiguous_pool dmaPool_;
	virtio_core::Queue *receiveVq_;
	virtio_core::Queue *transmitVq_;
};

VirtioNic::VirtioNic(std::unique_ptr<virtio_core::Transport> transport)
	: Nic(&dmaPool_), transport_ { std::move(transport) }
{
	if(transport_->checkDeviceFeature(VIRTIO_NET_F_MAC)) {
		for (int i = 0; i < 6; i++) {
			mac[i] = transport_->loadConfig8(i);
		}
		char ms[3 * 6 + 1];
		sprintf(ms, "%.2x:%.2x:%.2x:%.2x:%.2x:%.2x",
				mac[0], mac[1], mac[2],
				mac[3], mac[4], mac[5]);
		std::cout << "nic/virtio: Device has a hardware MAC: "
			<< ms << std::endl;
		transport_->acknowledgeDriverFeature(VIRTIO_NET_F_MAC);
	}

	transport_->finalizeFeatures();
	transport_->claimQueues(2);
	receiveVq_ = transport_->setupQueue(0);
	transmitVq_ = transport_->setupQueue(1);

	transport_->runDevice();
}

async::result<size_t> VirtioNic::receive(arch::dma_buffer_view frame) {
	arch::dma_object<VirtHeader> header { &dmaPool_ };

	virtio_core::Chain chain;
	chain.append(co_await receiveVq_->obtainDescriptor());
	chain.setupBuffer(virtio_core::deviceToHost,
			header.view_buffer().subview(0, legacyHeaderSize));
	chain.append(co_await receiveVq_->obtainDescriptor());
	chain.setupBuffer(virtio_core::deviceToHost, frame);

	co_await receiveVq_->submitDescriptor(chain.front());

	co_return mtu() + 14;
}

async::result<void> VirtioNic::send(const arch::dma_buffer_view payload) {
	if (payload.size() > 1514) {
		throw std::runtime_error("data exceeds mtu");
	}

	arch::dma_object<VirtHeader> header { &dmaPool_ };
	memset(header.data(), 0, sizeof(VirtHeader));

	virtio_core::Chain chain;
	chain.append(co_await transmitVq_->obtainDescriptor());
	chain.setupBuffer(virtio_core::hostToDevice,
			header.view_buffer().subview(0, legacyHeaderSize));
	chain.append(co_await transmitVq_->obtainDescriptor());
	chain.setupBuffer(virtio_core::hostToDevice, payload);

	if(logFrames) {
		std::cout << "virtio-driver: sending frame" << std::endl;
	}
	co_await transmitVq_->submitDescriptor(chain.front());
	if(logFrames) {
		std::cout << "virtio-driver: sent frame" << std::endl;
	}
}

} // namespace

namespace {
helix::UniqueLane netserverLane;
async::oneshot_event foundNetserver;
} // namespace

async::result<void> enumerateNetserver() {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("class", "netserver")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties) -> async::detached {
		netserverLane = helix::UniqueLane(co_await entity.bind());
		foundNetserver.raise();
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
	co_await foundNetserver.wait();
	co_return;
}

std::unordered_map<int64_t, std::shared_ptr<VirtioNic>> boundDeviceMap;

async::result<protocols::svrctl::Error> bindDevice(int64_t base_id) {
	std::cout << "nic/virtio: Binding to device " << base_id << std::endl;
	auto base_entity = co_await mbus::Instance::global().getEntity(base_id);

	// Do not bind to devices that are already bound to this driver.
	if(boundDeviceMap.find(base_entity.getId()) != boundDeviceMap.end())
		co_return protocols::svrctl::Error::success;

	// Make sure that we only bind to supported devices.
	auto properties = co_await base_entity.getProperties();
	if(auto vendor_str = std::get_if<mbus::StringItem>(&properties["pci-vendor"]);
			!vendor_str || vendor_str->value != "1af4")
		co_return protocols::svrctl::Error::deviceNotSupported;

	virtio_core::DiscoverMode discover_mode;
	if(auto device_str = std::get_if<mbus::StringItem>(&properties["pci-device"]); device_str) {
		if(device_str->value == "1000")
			discover_mode = virtio_core::DiscoverMode::transitional;
		else if(device_str->value == "1041")
			discover_mode = virtio_core::DiscoverMode::modernOnly;
		else
			co_return protocols::svrctl::Error::deviceNotSupported;
	}else{
		co_return protocols::svrctl::Error::deviceNotSupported;
	}

	protocols::hw::Device hwDevice(co_await base_entity.bind());
	co_await hwDevice.enableBusmaster();
	auto transport = co_await virtio_core::discover(std::move(hwDevice), discover_mode);
	auto dev = std::make_shared<VirtioNic>(std::move(transport));
	boundDeviceMap.insert({base_entity.getId(), dev});

	co_await nic_core::Nic::doBind(netserverLane, base_entity, std::move(dev));
	co_return protocols::svrctl::Error::success;
}

static constexpr protocols::svrctl::ControlOperations controlOps = {
	.bind = bindDevice
};

int main() {
	std::cout << "nic/virtio: Starting driver" << std::endl;

	async::run(enumerateNetserver(), helix::currentDispatcher);

	async::detach(protocols::svrctl::serveControl(&controlOps));
	async::run_forever(helix::currentDispatcher);
}
