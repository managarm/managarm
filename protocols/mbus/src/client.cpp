
#include <sys/auxv.h>
#include <iostream>

#include <helix/await.hpp>

#include <protocols/mbus/client.hpp>
#include "mbus.pb.h"

namespace mbus {
namespace _detail {

static Instance makeGlobal() {
	unsigned long server;
	if(peekauxval(AT_MBUS_SERVER, &server))
		throw std::runtime_error("No AT_MBUS_SERVER specified");
	return Instance(&helix::Dispatcher::global(), helix::BorrowedLane(server).dup());
}

Instance Instance::global() {
	static Instance instance(makeGlobal());
	return instance;
}

COFIBER_ROUTINE(async::result<Entity>, Instance::getRoot(), ([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvBuffer recv_resp;

	managarm::mbus::CntRequest req;
	req.set_req_type(managarm::mbus::CntReqType::GET_ROOT);

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];
	auto &&transmit = helix::submitAsync(_connection->lane, *_connection->dispatcher,
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, buffer, 128));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::mbus::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	COFIBER_RETURN(Entity(_connection, resp.id()));
}))

COFIBER_ROUTINE(cofiber::no_future, handleObject(std::shared_ptr<Connection> connection,
		std::function<async::result<helix::UniqueDescriptor>(AnyQuery)> handler,
		helix::UniqueLane p), ([connection, handler, lane = std::move(p)] {
	while(true) {
		helix::Accept accept;
		helix::RecvBuffer recv_req;

		char buffer[256];
		auto &&header = helix::submitAsync(lane, *connection->dispatcher,
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req, buffer, 256));
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();

		managarm::mbus::SvrRequest req;
		req.ParseFromArray(buffer, recv_req.actualLength());
		if(req.req_type() == managarm::mbus::SvrReqType::BIND) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor push_desc;

			auto descriptor = COFIBER_AWAIT handler(BindQuery());
			
			managarm::mbus::SvrResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, *connection->dispatcher,
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&push_desc, descriptor));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_desc.error());
		}else{
			throw std::runtime_error("Unexpected request type");
		}
	}
}))

COFIBER_ROUTINE(async::result<Entity>, Entity::createObject(std::string name,
		const Properties &properties,
		std::function<async::result<helix::UniqueDescriptor>(AnyQuery)> handler) const, ([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvBuffer recv_resp;
	helix::PullDescriptor pull_lane;

	managarm::mbus::CntRequest req;
	req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
	req.set_parent_id(_id);
	for(auto kv : properties)
		req.mutable_properties()->insert({ kv.first, kv.second });

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];
	auto &&transmit = helix::submitAsync(_connection->lane, *_connection->dispatcher,
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, buffer, 128, kHelItemChain),
			helix::action(&pull_lane));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_lane.error());

	managarm::mbus::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);
	
	handleObject(_connection, handler, helix::UniqueLane(pull_lane.descriptor()));

	COFIBER_RETURN(Entity(_connection, resp.id()));
}))

COFIBER_ROUTINE(cofiber::no_future, handleObserver(std::shared_ptr<Connection> connection,
		std::function<void(AnyEvent)> handler, helix::UniqueLane p),
		([connection, handler, lane = std::move(p)] {
	while(true) {
		helix::RecvBuffer recv_req;

		char buffer[256];
		auto &&header = helix::submitAsync(lane, *connection->dispatcher,
				helix::action(&recv_req, buffer, 256));
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(recv_req.error());

		managarm::mbus::SvrRequest req;
		req.ParseFromArray(buffer, recv_req.actualLength());
		if(req.req_type() == managarm::mbus::SvrReqType::ATTACH) {
			handler(AttachEvent(Entity(connection, req.id())));
		}else{
			throw std::runtime_error("Unexpected request type");
		}
	}
}))

static void encodeFilter(const AnyFilter &filter, managarm::mbus::AnyFilter *any_msg) {
	if(filter.type() == typeid(EqualsFilter)) {
		auto &real = boost::get<EqualsFilter>(filter);

		auto msg = any_msg->mutable_equals_filter();
		msg->set_path(real.getPath());
		msg->set_value(real.getValue());
	}else if(filter.type() == typeid(Conjunction)) {
		auto &real = boost::get<Conjunction>(filter);
		
		auto msg = any_msg->mutable_conjunction();
		for(auto &operand : real.getOperands())
			encodeFilter(operand, msg->add_operands());
	}else{
		throw std::runtime_error("Unexpected filter type");
	}
}

COFIBER_ROUTINE(async::result<Observer>, Entity::linkObserver(const AnyFilter &filter,
		std::function<void(AnyEvent)> handler) const, ([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvBuffer recv_resp;
	helix::PullDescriptor pull_lane;

	managarm::mbus::CntRequest req;
	req.set_req_type(managarm::mbus::CntReqType::LINK_OBSERVER);
	req.set_id(_id);
	encodeFilter(filter, req.mutable_filter());

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];
	auto &&transmit = helix::submitAsync(_connection->lane, *_connection->dispatcher,
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, buffer, 128, kHelItemChain),
			helix::action(&pull_lane));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_lane.error());

	managarm::mbus::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);
	
	handleObserver(_connection, handler, helix::UniqueLane(pull_lane.descriptor()));

	COFIBER_RETURN(Observer());
}))

COFIBER_ROUTINE(async::result<helix::UniqueDescriptor>, Entity::bind() const, ([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvBuffer recv_resp;
	helix::PullDescriptor pull_desc;

	managarm::mbus::CntRequest req;
	req.set_req_type(managarm::mbus::CntReqType::BIND2);
	req.set_id(_id);

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];
	auto &&transmit = helix::submitAsync(_connection->lane, *_connection->dispatcher,
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, buffer, 128, kHelItemChain),
			helix::action(&pull_desc));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_desc.error());

	managarm::mbus::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);
	
	COFIBER_RETURN(pull_desc.descriptor());
}))

} } // namespace mbus::_detail

