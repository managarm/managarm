#include <stdlib.h>
#include <string.h>
#include <iostream>

#include "console.hpp"

#include <async/jump.hpp>
#include <protocols/mbus/client.hpp>
#include <kerncfg.pb.h>

namespace {
	helix::UniqueLane kerncfgByteRingLane;
	async::jump foundKerncfgByteRing;
}

async::result<void> enumerateKerncfgByteRing(const char *purpose) {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("class", "kerncfg-byte-ring"),
		mbus::EqualsFilter("purpose", purpose)
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) -> async::detached {
		std::cout << "virtio-console: Found kerncfg" << std::endl;

		kerncfgByteRingLane = helix::UniqueLane(co_await entity.bind());
		foundKerncfgByteRing.trigger();
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
	co_await foundKerncfgByteRing.async_wait();
}

async::result<std::tuple<size_t, uint64_t, uint64_t>>
getKerncfgByteRingPart(arch::dma_buffer_view chunk, uint64_t dequeue) {
	managarm::kerncfg::CntRequest req;
	req.set_req_type(managarm::kerncfg::CntReqType::GET_BUFFER_CONTENTS);
	req.set_size(chunk.size());
	req.set_dequeue(dequeue);

	auto ser = req.SerializeAsString();
	auto [offer, send_req, recv_resp, recv_buffer] =
		co_await helix_ng::exchangeMsgs(kerncfgByteRingLane,
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvInline(),
				helix_ng::recvBuffer(chunk.data(), chunk.size())
			)
		);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(recv_buffer.error());

	managarm::kerncfg::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::kerncfg::Error::SUCCESS);

	co_return std::make_tuple(resp.size(), resp.new_dequeue(), resp.enqueue());
}

namespace tty {
namespace virtio_console {

// --------------------------------------------------------
// Device
// --------------------------------------------------------

Device::Device(std::unique_ptr<virtio_core::Transport> transport)
: transport_{std::move(transport)} { }

async::detached Device::runDevice() {
	transport_->finalizeFeatures();
	transport_->claimQueues(2);
	rxQueue_ = transport_->setupQueue(0);
	txQueue_ = transport_->setupQueue(1);

	auto maxPorts = transport_->space().load(spec::regs::maxPorts);
	std::cout << "virtio-console: Device supports " << maxPorts << " ports" << std::endl;

	transport_->runDevice();

	co_await enumerateKerncfgByteRing("heap-trace");

	uint64_t dequeue = 0;

	constexpr size_t chunk_size = 64 * 1024;
	arch::dma_buffer chunk_buffer{&dmaPool_, chunk_size};

	while (true) {
		auto [size, new_dequeue, enqueue] = co_await getKerncfgByteRingPart(chunk_buffer, dequeue);
		if (!size)
			continue;

		if (dequeue + (256 * 1024 * 1024) < enqueue)
			std::cerr << "virtio-console: warning, we possibly missed "
				<< (enqueue - dequeue + (256 * 1024 * 1024)) << " bytes" << std::endl;

		dequeue = new_dequeue;

		virtio_core::Chain chain;
		chain.append(co_await txQueue_->obtainDescriptor());
		chain.setupBuffer(virtio_core::hostToDevice, chunk_buffer.subview(0, size));
		co_await txQueue_->submitDescriptor(chain.front());
	}
}

} } // namespace tty::virtio_console
