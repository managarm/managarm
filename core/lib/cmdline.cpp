#include <bragi/helpers-std.hpp>
#include <core/cmdline.hpp>
#include <frg/cmdline.hpp>
#include <kerncfg.bragi.hpp>
#include <protocols/mbus/client.hpp>

async::result<std::string> Cmdline::get() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "kerncfg"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	auto lane = (co_await entity.getRemoteLane()).unwrap();

	managarm::kerncfg::GetCmdlineRequest req;

	auto [offer, sendReq, recvResp] =
		co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	auto resp = *bragi::parse_head_only<managarm::kerncfg::SvrResponse>(recvResp);
	assert(resp.error() == managarm::kerncfg::Error::SUCCESS);

	std::vector<char> buffer(resp.size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		offer.descriptor(),
		helix_ng::recvBuffer(buffer.data(), buffer.size())
	);

	HEL_CHECK(recv_tail.error());

	cmdline_ = std::string{buffer.data(), buffer.size()};

	co_return *cmdline_;
}

async::result<bool> Cmdline::dumpKernelLogs(frg::string_view driver) {
	if(!cmdline_.has_value())
		co_await get();

	frg::string_view dump = "none";

	frg::array args = {
		frg::option("serial.dump", frg::as_string_view(dump)),
	};

	frg::parse_arguments({cmdline_->data(), cmdline_->length()}, args);

	if(dump == "none")
		co_return false;

	if(dump == "all")
		co_return true;

	size_t next_item = 0;

	// poor man's tokenizer
	while(next_item < dump.size()) {
		if(next_item == dump.size() - 1)
			break;

		auto next_comma = dump.find_first(',', next_item);

		if(dump.sub_string(next_item, next_comma - next_item) == driver)
			co_return true;

		next_item = next_comma + 1;
	}

	co_return false;
}
