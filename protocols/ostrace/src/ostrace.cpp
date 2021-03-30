#include <async/oneshot-event.hpp>
#include <bragi/helpers-std.hpp>
#include <frg/std_compat.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/ostrace/ostrace.hpp>
#include <ostrace.bragi.hpp>

namespace protocols::ostrace {

Context::Context()
: enabled_{false} { }

Context::Context(helix::UniqueLane lane, bool enabled)
: lane_{std::move(lane)}, enabled_{enabled} { }

async::result<EventId> Context::announceEvent(std::string_view name) {
	managarm::ostrace::AnnounceEventReq req;
	req.set_name(std::string{name});

	auto [offer, sendReq, recvResp] =
		co_await helix_ng::exchangeMsgs(
			lane_,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	auto maybeResp = bragi::parse_head_only<managarm::ostrace::Response>(recvResp);
	assert(maybeResp);
	auto &resp = maybeResp.value();
	assert(resp.error() == managarm::ostrace::Error::SUCCESS);

	co_return EventId{resp.id()};
}

async::result<ItemId> Context::announceItem(std::string_view name) {
	managarm::ostrace::AnnounceItemReq req;
	req.set_name(std::string{name});

	auto [offer, sendReq, recvResp] =
		co_await helix_ng::exchangeMsgs(
			lane_,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	auto maybeResp = bragi::parse_head_only<managarm::ostrace::Response>(recvResp);
	assert(maybeResp);
	auto &resp = maybeResp.value();
	assert(resp.error() == managarm::ostrace::Error::SUCCESS);

	co_return ItemId{resp.id()};
}

Event::Event(Context *ctx, EventId id)
: ctx_{ctx} {
	live_ = ctx->isActive();
	req_.set_id(static_cast<uint64_t>(id));
}

void Event::withCounter(ItemId id, int64_t value) {
	if(!live_)
		return;

	managarm::ostrace::CounterItem item;
	item.set_id(static_cast<uint64_t>(id));
	item.set_value(value);
	req_.add_ctrs(std::move(item));
}

async::result<void> Event::emit() {
	if(!live_)
		co_return;

	auto [offer, sendReq, recvResp] =
		co_await helix_ng::exchangeMsgs(
			ctx_->getLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req_, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	auto maybeResp = bragi::parse_head_only<managarm::ostrace::Response>(recvResp);
	assert(maybeResp);
	auto &resp = maybeResp.value();
	assert(resp.error() == managarm::ostrace::Error::SUCCESS);
}

async::result<Context> createContext() {
	auto root = co_await mbus::Instance::global().getRoot();

	// Find ostrace in mbus.

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("class", "ostrace")
	});

	helix::UniqueLane lane;
	async::oneshot_event foundObject;

	auto handler = mbus::ObserverHandler{}
	.withAttach([&lane, &foundObject] (mbus::Entity entity, mbus::Properties properties)
			-> async::detached {
		std::cout << "ostrace: Found ostrace" << std::endl;

		lane = helix::UniqueLane(co_await entity.bind());
		foundObject.raise();
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
	co_await foundObject.wait();

	// Perform the negotiation request.

	managarm::ostrace::AnnounceItemReq req;

	auto [offer, sendReq, recvResp] =
		co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	auto maybeResp = bragi::parse_head_only<managarm::ostrace::Response>(recvResp);
	assert(maybeResp);
	auto &resp = maybeResp.value();

	if(resp.error() == managarm::ostrace::Error::OSTRACE_GLOBALLY_DISABLED)
		co_return Context{std::move(lane), false};

	assert(resp.error() == managarm::ostrace::Error::SUCCESS);
	co_return Context{std::move(lane), true};
}

} // namespace protocols::ostrace
