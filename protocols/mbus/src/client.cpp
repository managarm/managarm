
#include <iostream>

#include <protocols/mbus/client.hpp>
#include <protocols/posix/supercalls.hpp>
#include <protocols/posix/data.hpp>
#include <bragi/helpers-std.hpp>
#include <bragi/helpers-all.hpp>
#include "helix/ipc.hpp"
#include "mbus.bragi.hpp"

namespace {
	HelHandle getMbusClientLane() {
		posix::ManagarmProcessData data;

		HEL_CHECK(helSyscall1(kHelCallSuper + posix::superGetProcessData,
						reinterpret_cast<HelWord>(&data)));

		return data.mbusLane;
	}

	bool recreateInstance = false;
} // namespace anonymous

namespace mbus {

void recreateInstance() {
	::recreateInstance = true;
}

namespace _detail {

static Instance makeGlobal() {
	return Instance(helix::BorrowedLane(getMbusClientLane()).dup());
}

Instance Instance::global() {
	static Instance instance{makeGlobal()};

	if (::recreateInstance)
		instance = makeGlobal();

	return instance;
}

async::result<Entity> Instance::getRoot() {
	managarm::mbus::GetRootRequest req;

	auto [offer, sendReq, recvResp] =
		co_await helix_ng::exchangeMsgs(
			_connection->lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	auto resp = *bragi::parse_head_only<managarm::mbus::SvrResponse>(recvResp);

	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	co_return Entity{_connection, resp.id()};
}

async::result<Entity> Instance::getEntity(int64_t id) {
	co_return Entity{_connection, id};
}

async::detached handleObject(std::shared_ptr<Connection>,
		ObjectHandler handler, helix::UniqueLane lane) {
	while(true) {
		auto [accept, recvHead] = co_await helix_ng::exchangeMsgs(
				lane,
				helix_ng::accept(
					helix_ng::recvInline()
				)
			);

		HEL_CHECK(accept.error());
		HEL_CHECK(recvHead.error());

		auto conversation = accept.descriptor();

		// NOTE: S2CBindRequest does not have a tail, nor does it actually have
		//       any contents.

		auto preamble = bragi::read_preamble(recvHead);
		assert(!preamble.error());
		assert(preamble.id() == bragi::message_id<managarm::mbus::S2CBindRequest>);
		recvHead.reset();

		auto descriptor = co_await handler.bind();

		managarm::mbus::CntResponse resp;
		resp.set_error(managarm::mbus::Error::SUCCESS);

		auto [sendResp, pushDesc] =
			co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::pushDescriptor(descriptor)
			);

		HEL_CHECK(sendResp.error());
		HEL_CHECK(pushDesc.error());
	}
}

async::result<Properties> Entity::getProperties() const {
	managarm::mbus::GetPropertiesRequest req;
	req.set_id(_id);

	auto [offer, sendReq, recvHead] =
		co_await helix_ng::exchangeMsgs(
			_connection->lane,
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
	assert(!preamble.error());
	assert(preamble.id() == bragi::message_id<managarm::mbus::GetPropertiesResponse>);

	std::vector<std::byte> tail(preamble.tail_size());
	auto [recvTail] =
		co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(tail.data(), tail.size())
		);
	HEL_CHECK(recvTail.error());

	auto resp = *bragi::parse_head_tail<managarm::mbus::GetPropertiesResponse>(recvHead, tail);
	recvHead.reset();

	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	Properties properties;
	for(auto &kv : resp.properties())
		properties.insert({ kv.name(), StringItem{ kv.string_item() } });

	co_return properties;
}

async::result<Entity> Entity::createObject(std::string name,
		const Properties &properties, ObjectHandler handler) const {
	(void) name;

	managarm::mbus::CreateObjectRequest req;
	req.set_parent_id(_id);

	for(auto kv : properties) {
		managarm::mbus::Property prop;
		prop.set_name(kv.first);
		prop.set_string_item(std::get<StringItem>(kv.second).value);
		req.add_properties(prop);
	}

	auto [offer, sendHead, sendTail, recvResp, pullLane] =
		co_await helix_ng::exchangeMsgs(
			_connection->lane,
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

	auto resp = *bragi::parse_head_only<managarm::mbus::SvrResponse>(recvResp);
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	auto lane = pullLane.descriptor();
	handleObject(_connection, handler, std::move(lane));

	co_return Entity{_connection, resp.id()};
}

async::detached handleObserver(std::shared_ptr<Connection> connection,
		ObserverHandler handler, AnyFilter filter) {
	uint64_t curSeq = 0;

	while(true) {
		auto [outSeq, actualSeq, entities] =
			co_await connection->enumerate(curSeq, filter);

		curSeq = outSeq;
		for (auto &entity : entities) {
			handler.attach(Entity{connection, entity.id}, std::move(entity.properties));
		}
	}
}

static void encodeFilter(const AnyFilter &filter, auto &msg) {
	managarm::mbus::AnyFilter flt;
	if(auto alt = std::get_if<EqualsFilter>(&filter); alt) {
		managarm::mbus::EqualsFilter eqf;
		eqf.set_path(alt->getPath());
		eqf.set_value(alt->getValue());
		flt.set_equals_filter(std::move(eqf));
	}else if(auto alt = std::get_if<Conjunction>(&filter); alt) {
		managarm::mbus::Conjunction conj;
		for(auto &operand : alt->getOperands()) {
			auto eqf = std::get_if<EqualsFilter>(&operand);
			assert(eqf && "Sorry, unimplemented: Non-EqualsFilter in Conjunction");

			managarm::mbus::EqualsFilter flt{};

			flt.set_path(eqf->getPath());
			flt.set_value(eqf->getValue());
			conj.add_operands(std::move(flt));
		}
		flt.set_conjunction(std::move(conj));
	}else{
		throw std::runtime_error("Unexpected filter type");
	}

	msg.set_filter(std::move(flt));
}

async::result<Observer> Entity::linkObserver(const AnyFilter &filter,
		ObserverHandler handler) const {
	handleObserver(_connection, handler, filter);
	co_return Observer();
}

async::result<Connection::EnumerationResult> Connection::enumerate(uint64_t seq,
		const AnyFilter &filter) const {
	managarm::mbus::EnumerateRequest req;
	req.set_seq(seq);
	encodeFilter(filter, req);

	auto [offer, sendHead, sendTail, recvRespHead] =
		co_await helix_ng::exchangeMsgs(
			lane,
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
	assert(!preamble.error());
	assert(preamble.id() == bragi::message_id<managarm::mbus::EnumerateResponse>);

	std::vector<std::byte> tail(preamble.tail_size());
	auto [recvRespTail] =
		co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(tail.data(), tail.size())
		);
	HEL_CHECK(recvRespTail.error());

	auto resp = *bragi::parse_head_tail<managarm::mbus::EnumerateResponse>(recvRespHead, tail);
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	recvRespHead.reset();

	EnumerationResult result{};
	result.outSeq = resp.out_seq();
	result.actualSeq = resp.actual_seq();

	for (auto &entity : resp.entities()) {
		EnumeratedEntity enumEntity{};
		enumEntity.id = entity.id();
		for(auto &kv : entity.properties())
			enumEntity.properties.insert({ kv.name(), StringItem{ kv.string_item() } });
		result.entities.push_back(enumEntity);
	}

	co_return result;
}

async::result<helix::UniqueDescriptor> Entity::bind() const {
	managarm::mbus::GetRemoteLaneRequest req;
	req.set_id(_id);

	auto [offer, sendReq, recvResp, pullLane] =
		co_await helix_ng::exchangeMsgs(
			_connection->lane,
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

	auto resp = *bragi::parse_head_only<managarm::mbus::GetRemoteLaneResponse>(recvResp);
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	co_return pullLane.descriptor();
}

} } // namespace mbus::_detail

