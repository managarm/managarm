
#include <sys/auxv.h>
#include <iostream>

#include <protocols/mbus/client.hpp>
#include "mbus.pb.h"

namespace mbus {
namespace _detail {

static Instance makeGlobal() {
	unsigned long server;
	if(peekauxval(AT_MBUS_SERVER, &server))
		throw std::runtime_error("No AT_MBUS_SERVER specified");
	return Instance(helix::BorrowedLane(server).dup());
}

Instance Instance::global() {
	static Instance instance(makeGlobal());
	return instance;
}

async::result<Entity> Instance::getRoot() {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvBuffer recv_resp;

	managarm::mbus::CntRequest req;
	req.set_req_type(managarm::mbus::CntReqType::GET_ROOT);

	auto ser = req.SerializeAsString();
	uint8_t buffer[1024];
	auto &&transmit = helix::submitAsync(_connection->lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, buffer, 1024));
	co_await transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::mbus::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	co_return Entity{_connection, resp.id()};
}

async::result<Entity> Instance::getEntity(int64_t id) {
	co_return Entity{_connection, id};
}

async::detached handleObject(std::shared_ptr<Connection> connection,
		ObjectHandler handler, helix::UniqueLane lane) {
	while(true) {
		helix::Accept accept;
		helix::RecvBuffer recv_req;

		char buffer[1024];
		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req, buffer, 1024));
		co_await header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();

		managarm::mbus::SvrRequest req;
		req.ParseFromArray(buffer, recv_req.actualLength());
		if(req.req_type() == managarm::mbus::SvrReqType::BIND) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor push_desc;

			auto descriptor = co_await handler.bind();

			managarm::mbus::SvrResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&push_desc, descriptor));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_desc.error());
		}else{
			throw std::runtime_error("Unexpected request type");
		}
	}
}

async::result<Properties> Entity::getProperties() const {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvBuffer recv_resp;

	managarm::mbus::CntRequest req;
	req.set_req_type(managarm::mbus::CntReqType::GET_PROPERTIES);
	req.set_id(_id);

	auto ser = req.SerializeAsString();
	uint8_t buffer[1024];
	auto &&transmit = helix::submitAsync(_connection->lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, buffer, 1024));
	co_await transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::mbus::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	Properties properties;
	for(auto &kv : resp.properties())
		properties.insert({ kv.name(), StringItem{kv.item().string_item().value()} });
	co_return properties;
}

async::result<Entity> Entity::createObject(std::string name,
		const Properties &properties, ObjectHandler handler) const {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvBuffer recv_resp;
	helix::PullDescriptor pull_lane;

	managarm::mbus::CntRequest req;
	req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
	req.set_parent_id(_id);
	for(auto kv : properties) {
		auto entry = req.add_properties();
		entry->set_name(kv.first);
		entry->mutable_item()->mutable_string_item()->set_value(std::get<StringItem>(kv.second).value);
	}

	auto ser = req.SerializeAsString();
	uint8_t buffer[1024];
	auto &&transmit = helix::submitAsync(_connection->lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, buffer, 1024, kHelItemChain),
			helix::action(&pull_lane));
	co_await transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_lane.error());

	managarm::mbus::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	handleObject(_connection, handler, helix::UniqueLane(pull_lane.descriptor()));

	co_return Entity{_connection, resp.id()};
}

async::detached handleObserver(std::shared_ptr<Connection> connection,
		ObserverHandler handler, helix::UniqueLane lane) {
	while(true) {
		helix::RecvBuffer recv_req;

		char buffer[1024];
		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&recv_req, buffer, 1024));
		co_await header.async_wait();
		HEL_CHECK(recv_req.error());

		managarm::mbus::SvrRequest req;
		req.ParseFromArray(buffer, recv_req.actualLength());
		if(req.req_type() == managarm::mbus::SvrReqType::ATTACH) {
			Properties properties;
			for(auto &kv : req.properties())
				properties.insert({ kv.name(), StringItem{kv.item().string_item().value()} });

			handler.attach(Entity{connection, req.id()}, std::move(properties));
		}else{
			throw std::runtime_error("Unexpected request type");
		}
	}
}

static void encodeFilter(const AnyFilter &filter, managarm::mbus::AnyFilter *any_msg) {
	if(auto alt = std::get_if<EqualsFilter>(&filter); alt) {
		auto msg = any_msg->mutable_equals_filter();
		msg->set_path(alt->getPath());
		msg->set_value(alt->getValue());
	}else if(auto alt = std::get_if<Conjunction>(&filter); alt) {
		auto msg = any_msg->mutable_conjunction();
		for(auto &operand : alt->getOperands())
			encodeFilter(operand, msg->add_operands());
	}else{
		throw std::runtime_error("Unexpected filter type");
	}
}

async::result<Observer> Entity::linkObserver(const AnyFilter &filter,
		ObserverHandler handler) const {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvBuffer recv_resp;
	helix::PullDescriptor pull_lane;

	managarm::mbus::CntRequest req;
	req.set_req_type(managarm::mbus::CntReqType::LINK_OBSERVER);
	req.set_id(_id);
	encodeFilter(filter, req.mutable_filter());

	auto ser = req.SerializeAsString();
	uint8_t buffer[1024];
	auto &&transmit = helix::submitAsync(_connection->lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, buffer, 1024, kHelItemChain),
			helix::action(&pull_lane));
	co_await transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_lane.error());

	managarm::mbus::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	handleObserver(_connection, handler, helix::UniqueLane(pull_lane.descriptor()));

	co_return Observer();
}

async::result<helix::UniqueDescriptor> Entity::bind() const {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvBuffer recv_resp;
	helix::PullDescriptor pull_desc;

	managarm::mbus::CntRequest req;
	req.set_req_type(managarm::mbus::CntReqType::BIND2);
	req.set_id(_id);

	auto ser = req.SerializeAsString();
	uint8_t buffer[1024];
	auto &&transmit = helix::submitAsync(_connection->lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, buffer, 1024, kHelItemChain),
			helix::action(&pull_desc));
	co_await transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_desc.error());

	managarm::mbus::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	co_return pull_desc.descriptor();
}

} } // namespace mbus::_detail

