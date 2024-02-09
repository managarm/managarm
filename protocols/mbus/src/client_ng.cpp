
#include <iostream>

#include <protocols/mbus/client.hpp>
#include <protocols/posix/supercalls.hpp>
#include <protocols/posix/data.hpp>
#include <bragi/helpers-std.hpp>
#include <bragi/helpers-all.hpp>
#include "helix/ipc-structs.hpp"
#include "helix/ipc.hpp"
#include "mbus.bragi.hpp"

#include <span>

namespace {
	HelHandle getMbusClientLane() {
		posix::ManagarmProcessData data;

		HEL_CHECK(helSyscall1(kHelCallSuper + posix::superGetProcessData,
						reinterpret_cast<HelWord>(&data)));

		return data.mbusLane;
	}

	bool recreateInstance = false;

	static mbus_ng::Instance makeGlobal() {
		return mbus_ng::Instance(helix::BorrowedLane(getMbusClientLane()).dup());
	}
} // namespace anonymous

namespace mbus_ng {

void recreateInstance() {
	::recreateInstance = true;
}

Instance Instance::global() {
	static Instance instance{makeGlobal()};

	if (::recreateInstance)
		instance = makeGlobal();

	return instance;
}

async::result<Entity> Instance::getEntity(int64_t id) {
	co_return Entity{connection_, id};
}

async::result<Result<EntityManager>>
Instance::createEntity(std::string_view name, const Properties &properties) {
	managarm::mbus::CreateObjectRequest req;
	req.set_name(std::string{name});

	for(auto kv : properties) {
		managarm::mbus::Property prop;
		prop.set_name(kv.first);
		prop.set_string_item(std::get<StringItem>(kv.second).value);
		req.add_properties(prop);
	}

	auto [offer, sendHead, sendTail, recvResp, pullLane] =
		co_await helix_ng::exchangeMsgs(
			connection_->lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendHead.error());
	HEL_CHECK(sendTail.error());
	HEL_CHECK(recvResp.error());
	HEL_CHECK(pullLane.error());

	auto maybeResp = bragi::parse_head_only<managarm::mbus::CreateObjectResponse>(recvResp);
	if (!maybeResp)
		co_return Error::protocolViolation;

	auto &resp = *maybeResp;
	if (resp.error() == managarm::mbus::Error::NO_SUCH_ENTITY)
		co_return Error::noSuchEntity;

	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	co_return EntityManager{resp.id(), pullLane.descriptor()};
}

// ------------------------------------------------------------------------
// mbus Entity class.
// ------------------------------------------------------------------------

async::result<Result<Properties>> Entity::getProperties() const {
	managarm::mbus::GetPropertiesRequest req;
	req.set_id(id_);

	auto [offer, sendReq, recvHead] =
		co_await helix_ng::exchangeMsgs(
			connection_->lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	auto conversation = offer.descriptor();

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvHead.error());

	auto preamble = bragi::read_preamble(recvHead);
	if (preamble.error() || preamble.id() != bragi::message_id<managarm::mbus::GetPropertiesResponse>)
		co_return Error::protocolViolation;

	std::vector<std::byte> tail(preamble.tail_size());
	auto [recvTail] =
		co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(tail.data(), tail.size())
		);
	HEL_CHECK(recvTail.error());

	auto maybeResp = bragi::parse_head_tail<managarm::mbus::GetPropertiesResponse>(recvHead, tail);
	if (!maybeResp)
		co_return Error::protocolViolation;

	auto &resp = *maybeResp;
	if (resp.error() == managarm::mbus::Error::NO_SUCH_ENTITY)
		co_return Error::noSuchEntity;

	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	Properties properties;
	for(auto &kv : resp.properties())
		properties.insert({ kv.name(), StringItem{ kv.string_item() } });

	co_return properties;
}

async::result<Result<helix::UniqueLane>> Entity::getRemoteLane() const {
	managarm::mbus::GetRemoteLaneRequest req;
	req.set_id(id_);

	auto [offer, sendReq, recvResp, pullLane] =
		co_await helix_ng::exchangeMsgs(
			connection_->lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());
	HEL_CHECK(pullLane.error());

	auto maybeResp = bragi::parse_head_only<managarm::mbus::GetRemoteLaneResponse>(recvResp);
	if (!maybeResp)
		co_return Error::protocolViolation;

	auto &resp = *maybeResp;
	if (resp.error() == managarm::mbus::Error::NO_SUCH_ENTITY)
		co_return Error::noSuchEntity;

	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	co_return pullLane.descriptor();
}

// ------------------------------------------------------------------------
// mbus EntityManager class.
// ------------------------------------------------------------------------

async::result<Result<void>> EntityManager::serveRemoteLane(helix::UniqueLane lane) const {
	managarm::mbus::ServeRemoteLaneRequest req;

	auto [offer, sendReq, pushLane, recvResp] =
		co_await helix_ng::exchangeMsgs(
			mgmtLane_,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::pushDescriptor(lane),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(pushLane.error());
	HEL_CHECK(recvResp.error());

	auto preamble = bragi::read_preamble(recvResp);
	if (preamble.error() || preamble.id() != bragi::message_id<managarm::mbus::ServeRemoteLaneResponse>)
		co_return Error::protocolViolation;

	auto maybeResp = bragi::parse_head_only<managarm::mbus::ServeRemoteLaneResponse>(recvResp);
	if (!maybeResp)
		co_return Error::protocolViolation;

	auto &resp = *maybeResp;
	if (resp.error() == managarm::mbus::Error::NO_SUCH_ENTITY)
		co_return Error::noSuchEntity;

	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	co_return frg::success;
}

// ------------------------------------------------------------------------
// mbus Enumerator class.
// ------------------------------------------------------------------------

static void encodeFilter(const AnyFilter &filter, auto &msg) {
	managarm::mbus::AnyFilter flt;
	if(auto alt = std::get_if<EqualsFilter>(&filter); alt) {
		managarm::mbus::EqualsFilter eqf;
		eqf.set_path(alt->path());
		eqf.set_value(alt->value());
		flt.set_equals_filter(std::move(eqf));
	}else if(auto alt = std::get_if<Conjunction>(&filter); alt) {
		managarm::mbus::Conjunction conj;
		for(auto &operand : alt->operands()) {
			auto eqf = std::get_if<EqualsFilter>(&operand);
			assert(eqf && "Sorry, unimplemented: Non-EqualsFilter in Conjunction");

			managarm::mbus::EqualsFilter flt{};

			flt.set_path(eqf->path());
			flt.set_value(eqf->value());
			conj.add_operands(std::move(flt));
		}
		flt.set_conjunction(std::move(conj));
	}else{
		throw std::runtime_error("Unexpected filter type");
	}

	msg.set_filter(std::move(flt));
}

async::result<Result<EnumerationResult>> Enumerator::nextEvents() {
	managarm::mbus::EnumerateRequest req;
	req.set_seq(curSeq_);
	encodeFilter(filter_, req);

	auto [offer, sendHead, sendTail, recvRespHead] =
		co_await helix_ng::exchangeMsgs(
			connection_->lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendHead.error());
	HEL_CHECK(sendTail.error());
	HEL_CHECK(recvRespHead.error());

	auto conversation = offer.descriptor();

	auto preamble = bragi::read_preamble(recvRespHead);
	if (preamble.error() || preamble.id() != bragi::message_id<managarm::mbus::EnumerateResponse>)
		co_return Error::protocolViolation;

	std::vector<std::byte> tail(preamble.tail_size());
	auto [recvRespTail] =
		co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(tail.data(), tail.size())
		);
	HEL_CHECK(recvRespTail.error());

	auto maybeResp = bragi::parse_head_tail<managarm::mbus::EnumerateResponse>(recvRespHead, tail);
	if (!maybeResp)
		co_return Error::protocolViolation;

	auto &resp = *maybeResp;
	if (resp.error() == managarm::mbus::Error::NO_SUCH_ENTITY)
		co_return Error::noSuchEntity;

	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	EnumerationResult result{};
	result.paginated = resp.out_seq() != resp.actual_seq();

	for (auto &entity : resp.entities()) {
		using enum EnumerationEvent::Type;

		auto [_, unseen] = seenIds_.insert(entity.id());

		EnumerationEvent event{};
		event.id = entity.id();
		event.name = entity.name();
		event.type = unseen ? created : propertiesChanged;
		for(auto &kv : entity.properties())
			event.properties.insert({ kv.name(), StringItem{ kv.string_item() } });

		result.events.push_back(std::move(event));
	}

	curSeq_ = resp.out_seq();

	co_return result;
}

}  // namespace mbus_ng
