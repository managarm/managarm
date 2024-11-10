#include <bragi/helpers-std.hpp>
#include <core/kernel-logs.hpp>
#include <protocols/mbus/client.hpp>
#include <kerncfg.bragi.hpp>

async::result<void> KernelLogs::setupKerncfg() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "kerncfg-byte-ring"},
		mbus_ng::EqualsFilter{"purpose", "kernel-log"},
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	kerncfgLane_ = (co_await entity.getRemoteLane()).unwrap();
}

async::result<size_t> KernelLogs::getMessage(std::span<uint8_t> buffer) {
	if(!kerncfgLane_.has_value())
		co_await setupKerncfg();

	managarm::kerncfg::GetBufferContentsRequest req;
	req.set_size(buffer.size());
	req.set_dequeue(offset_);
	req.set_flags(managarm::kerncfg::GetBufferContentsFlags::ONE_RECORD);

	auto [offer, sendReq, recvResp, recvBuffer] =
		co_await helix_ng::exchangeMsgs(*kerncfgLane_,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::recvBuffer(buffer.data(), buffer.size())
			)
		);
	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());
	HEL_CHECK(recvBuffer.error());

	auto resp = *bragi::parse_head_only<managarm::kerncfg::SvrResponse>(recvResp);
	assert(resp.error() == managarm::kerncfg::Error::SUCCESS);

	assert(offset_ == resp.effective_dequeue());
	offset_ = resp.new_dequeue();

	co_return strnlen(reinterpret_cast<char *>(buffer.data()), req.size());
}
