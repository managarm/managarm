
#include <sys/auxv.h>
#include <iostream>

#include <helix/await.hpp>

#include <mbus.hpp>
#include "mbus.pb.h"

using M = helix::AwaitMechanism;

namespace mbus {
namespace _detail {

static Instance makeGlobal() {
	unsigned long server;
	if(peekauxval(AT_MBUS_SERVER, &server))
		throw std::runtime_error("No AT_MBUS_SERVER specified");
	return Instance(&helix::Dispatcher::global(), helix::BorrowedPipe(server).dup());
}

Instance Instance::global() {
	static Instance instance(makeGlobal());
	return instance;
}

COFIBER_ROUTINE(cofiber::future<Entity>, Instance::getRoot(), ([=] {
	helix::Offer<M> offer;
	helix::SendBuffer<M> send_req;
	helix::RecvBuffer<M> recv_resp;

	managarm::mbus::CntRequest req;
	req.set_req_type(managarm::mbus::CntReqType::GET_ROOT);

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];
	helix::submitAsync(_connection->pipe, {
		helix::action(&offer, kHelItemAncillary),
		helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
		helix::action(&recv_resp, buffer, 128)
	}, *_connection->dispatcher);

	COFIBER_AWAIT offer.future();
	COFIBER_AWAIT send_req.future();
	COFIBER_AWAIT recv_resp.future();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::mbus::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	COFIBER_RETURN(Entity(_connection, resp.id()));
}))

COFIBER_ROUTINE(cofiber::no_future, handleObject(std::shared_ptr<Connection> connection,
		std::function<cofiber::future<helix::UniqueDescriptor>(AnyQuery)> handler,
		helix::UniquePipe p), ([connection, handler, lane = std::move(p)] {
	using M = helix::AwaitMechanism;

	while(true) {
		helix::Accept<M> accept;
		helix::RecvBuffer<M> recv_req;

		char buffer[256];
		helix::submitAsync(lane, {
			helix::action(&accept, kHelItemAncillary),
			helix::action(&recv_req, buffer, 256)
		}, *connection->dispatcher);

		COFIBER_AWAIT accept.future();
		COFIBER_AWAIT recv_req.future();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();

		managarm::mbus::SvrRequest req;
		req.ParseFromArray(buffer, recv_req.actualLength());
		if(req.req_type() == managarm::mbus::SvrReqType::BIND) {
			helix::SendBuffer<M> send_resp;
			helix::PushDescriptor<M> push_desc;

			auto descriptor = COFIBER_AWAIT handler(BindQuery());
			
			managarm::mbus::SvrResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&push_desc, descriptor),
			}, *connection->dispatcher);
			
			COFIBER_AWAIT send_resp.future();
			COFIBER_AWAIT push_desc.future();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_desc.error());
		}else{
			throw std::runtime_error("Unexpected request type");
		}
	}
}))

COFIBER_ROUTINE(cofiber::future<Entity>, Entity::createObject(std::string name,
		const Properties &properties,
		std::function<cofiber::future<helix::UniqueDescriptor>(AnyQuery)> handler) const, ([=] {
	helix::Offer<M> offer;
	helix::SendBuffer<M> send_req;
	helix::RecvBuffer<M> recv_resp;
	helix::PullDescriptor<M> pull_lane;

	managarm::mbus::CntRequest req;
	req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
	req.set_parent_id(_id);
	for(auto kv : properties)
		req.mutable_properties()->insert({ kv.first, kv.second });

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];
	helix::submitAsync(_connection->pipe, {
		helix::action(&offer, kHelItemAncillary),
		helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
		helix::action(&recv_resp, buffer, 128, kHelItemChain),
		helix::action(&pull_lane),
	}, *_connection->dispatcher);
	
	COFIBER_AWAIT offer.future();
	COFIBER_AWAIT send_req.future();
	COFIBER_AWAIT recv_resp.future();
	COFIBER_AWAIT pull_lane.future();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_lane.error());

	managarm::mbus::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);
	
	handleObject(_connection, handler, helix::UniquePipe(pull_lane.descriptor()));

	COFIBER_RETURN(Entity(_connection, resp.id()));
}))

COFIBER_ROUTINE(cofiber::no_future, handleObserver(std::shared_ptr<Connection> connection,
		std::function<void(AnyEvent)> handler, helix::UniquePipe p),
		([connection, handler, lane = std::move(p)] {
	using M = helix::AwaitMechanism;

	while(true) {
		helix::RecvBuffer<M> recv_req;

		char buffer[256];
		helix::submitAsync(lane, {
			helix::action(&recv_req, buffer, 256)
		}, *connection->dispatcher);
		
		COFIBER_AWAIT recv_req.future();

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

COFIBER_ROUTINE(cofiber::future<Observer>, Entity::linkObserver(const AnyFilter &filter,
		std::function<void(AnyEvent)> handler) const, ([=] {
	helix::Offer<M> offer;
	helix::SendBuffer<M> send_req;
	helix::RecvBuffer<M> recv_resp;
	helix::PullDescriptor<M> pull_lane;

	managarm::mbus::CntRequest req;
	req.set_req_type(managarm::mbus::CntReqType::LINK_OBSERVER);
	req.set_id(_id);
	encodeFilter(filter, req.mutable_filter());

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];
	helix::submitAsync(_connection->pipe, {
		helix::action(&offer, kHelItemAncillary),
		helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
		helix::action(&recv_resp, buffer, 128, kHelItemChain),
		helix::action(&pull_lane),
	}, *_connection->dispatcher);
	
	COFIBER_AWAIT offer.future();
	COFIBER_AWAIT send_req.future();
	COFIBER_AWAIT recv_resp.future();
	COFIBER_AWAIT pull_lane.future();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_lane.error());

	managarm::mbus::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);
	
	handleObserver(_connection, handler, helix::UniquePipe(pull_lane.descriptor()));

	COFIBER_RETURN(Observer());
}))

COFIBER_ROUTINE(cofiber::future<helix::UniqueDescriptor>, Entity::bind() const, ([=] {
	helix::Offer<M> offer;
	helix::SendBuffer<M> send_req;
	helix::RecvBuffer<M> recv_resp;
	helix::PullDescriptor<M> pull_desc;

	managarm::mbus::CntRequest req;
	req.set_req_type(managarm::mbus::CntReqType::BIND2);
	req.set_id(_id);

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];
	helix::submitAsync(_connection->pipe, {
		helix::action(&offer, kHelItemAncillary),
		helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
		helix::action(&recv_resp, buffer, 128, kHelItemChain),
		helix::action(&pull_desc),
	}, *_connection->dispatcher);
	
	COFIBER_AWAIT offer.future();
	COFIBER_AWAIT send_req.future();
	COFIBER_AWAIT recv_resp.future();
	COFIBER_AWAIT pull_desc.future();
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

