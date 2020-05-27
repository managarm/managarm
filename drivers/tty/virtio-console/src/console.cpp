#include <stdlib.h>
#include <string.h>
#include <iostream>

#include "console.hpp"

#include <async/jump.hpp>
#include <protocols/mbus/client.hpp>
#include <kerncfg.pb.h>

async::result<helix::UniqueLane> enumerateKerncfgByteRing(const char *purpose) {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("class", "kerncfg-byte-ring"),
		mbus::EqualsFilter("purpose", purpose)
	});

	// TODO: this is quite dirty and we are leaking this async::promise.
	auto p = new async::promise<helix::UniqueLane>{};

	auto handler = mbus::ObserverHandler{}
	.withAttach([p] (mbus::Entity entity,
			mbus::Properties properties) mutable -> async::detached {
		std::cout << "virtio-console: Found kerncfg" << std::endl;
		p->set_value(helix::UniqueLane(co_await entity.bind()));
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
	co_return co_await p->async_get();
}

async::result<std::tuple<size_t, uint64_t, uint64_t>>
getKerncfgByteRingPart(helix::BorrowedLane lane,
		arch::dma_buffer_view chunk, uint64_t dequeue, uint64_t watermark) {
	managarm::kerncfg::CntRequest req;
	req.set_req_type(managarm::kerncfg::CntReqType::GET_BUFFER_CONTENTS);
	req.set_watermark(watermark);
	req.set_size(chunk.size());
	req.set_dequeue(dequeue);

	auto ser = req.SerializeAsString();
	auto [offer, send_req, recv_resp, recv_buffer] =
		co_await helix_ng::exchangeMsgs(lane,
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

	auto dumpKerncfgRing = [this] (const char *name, size_t watermark) -> async::result<void> {
		auto lane = co_await enumerateKerncfgByteRing(name);

		uint64_t dequeue = 0;

		arch::dma_buffer chunkBuffer{&dmaPool_, 1 << 16};

		while (true) {
			auto [size, newDequeue, enqueue] = co_await getKerncfgByteRingPart(
					lane, chunkBuffer, dequeue, watermark);
			if (!size)
				continue;

			// TODO: improve this by passing the "true" dequeue pointer to userspace.
			if (dequeue + (256 * 1024 * 1024) < enqueue)
				std::cerr << "virtio-console: warning, we possibly missed "
					<< (enqueue - dequeue + (256 * 1024 * 1024)) << " bytes" << std::endl;

			dequeue = newDequeue;

			virtio_core::Chain chain;
			chain.append(co_await txQueue_->obtainDescriptor());
			chain.setupBuffer(virtio_core::hostToDevice, chunkBuffer.subview(0, size));
			co_await txQueue_->submitDescriptor(chain.front());
		}
	};

	async::detach(dumpKerncfgRing("heap-trace", 1024 * 1024));
	async::detach(dumpKerncfgRing("kernel-profile", 1024));
	co_return;
}

} } // namespace tty::virtio_console
