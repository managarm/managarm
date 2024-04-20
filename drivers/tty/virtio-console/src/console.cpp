#include <stdlib.h>
#include <string.h>
#include <iostream>

#include "console.hpp"

#include <async/oneshot-event.hpp>
#include <async/promise.hpp>
#include <frg/std_compat.hpp>
#include <protocols/mbus/client.hpp>
#include <bragi/helpers-std.hpp>
#include <kerncfg.bragi.hpp>

async::result<helix::UniqueLane> enumerateKerncfgByteRing(const char *purpose) {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "kerncfg-byte-ring"},
		mbus_ng::EqualsFilter{"purpose", purpose},
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	std::cout << "virtio-console: Found kerncfg" << std::endl;
	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	co_return (co_await entity.getRemoteLane()).unwrap();
}

async::result<std::tuple<size_t, uint64_t, uint64_t>>
getKerncfgByteRingPart(helix::BorrowedLane lane,
		arch::dma_buffer_view chunk, uint64_t dequeue, uint64_t watermark) {
	managarm::kerncfg::GetBufferContentsRequest req;
	req.set_watermark(watermark);
	req.set_size(chunk.size());
	req.set_dequeue(dequeue);

	auto [offer, sendReq, recvResp, recvBuffer] =
		co_await helix_ng::exchangeMsgs(lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::recvBuffer(chunk.data(), chunk.size())
			)
		);
	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());
	HEL_CHECK(recvBuffer.error());

	auto resp = *bragi::parse_head_only<managarm::kerncfg::SvrResponse>(recvResp);
	assert(resp.error() == managarm::kerncfg::Error::SUCCESS);

	recvResp.reset();

	co_return std::make_tuple(resp.size(), resp.effective_dequeue(), resp.new_dequeue());
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
			auto [size, effectiveDequeue, newDequeue] = co_await getKerncfgByteRingPart(
					lane, chunkBuffer, dequeue, watermark);

			// TODO: improve this by passing the "true" dequeue pointer to userspace.
			if (dequeue != effectiveDequeue)
				std::cerr << "virtio-console: warning, we possibly missed "
					<< (effectiveDequeue - dequeue) << " bytes" << std::endl;

			dequeue = newDequeue;

			virtio_core::Chain chain;
			chain.append(co_await txQueue_->obtainDescriptor());
			chain.setupBuffer(virtio_core::hostToDevice, chunkBuffer.subview(0, size));
			co_await txQueue_->submitDescriptor(chain.front());
		}
	};

	async::detach(dumpKerncfgRing("heap-trace", 1024 * 1024));
	async::detach(dumpKerncfgRing("kernel-profile", 1024));
	async::detach(dumpKerncfgRing("os-trace", 1024));
	co_return;
}

} } // namespace tty::virtio_console
