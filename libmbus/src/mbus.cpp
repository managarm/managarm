
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
	helix::BorrowedHub hub = helix::Dispatcher::global().getHub();
	return Instance(helix::Dispatcher(hub.dup()), helix::BorrowedPipe(server).dup());
}

Instance Instance::global() {
	static Instance instance(makeGlobal());
	return instance;
}

COFIBER_ROUTINE(cofiber::future<Entity>, Instance::getRoot(), ([=] {
	managarm::mbus::CntRequest req;
	req.set_req_type(managarm::mbus::CntReqType::GET_ROOT);

	auto serialized = req.SerializeAsString();
	helix::SendString<M> send_req(_connection->dispatcher, _connection->pipe,
			serialized.data(), serialized.size(), 0, 0, kHelRequest);
	COFIBER_AWAIT send_req.future();
	HEL_CHECK(send_req.error());

	// recevie and parse the response.
	uint8_t buffer[128];
	helix::RecvString<M> recv_resp(_connection->dispatcher, _connection->pipe,
			buffer, 128, 0, 0, kHelResponse);
	COFIBER_AWAIT recv_resp.future();
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
		char buffer[128];
		helix::RecvString<M> recv_req(helix::Dispatcher::global(), lane,
				buffer, 128, 0, 0, kHelRequest);
		COFIBER_AWAIT recv_req.future();
		HEL_CHECK(recv_req.error());

		managarm::mbus::SvrRequest req;
		req.ParseFromArray(buffer, recv_req.actualLength());
		if(req.req_type() == managarm::mbus::SvrReqType::BIND) {
			auto descriptor = COFIBER_AWAIT handler(BindQuery());
			std::cout << "libmbus: Sending descriptor" << std::endl;
			
			managarm::mbus::SvrResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);

			auto serialized = resp.SerializeAsString();
			helix::SendString<M> send_resp(helix::Dispatcher::global(), lane,
					serialized.data(), serialized.size(), 0, 0, kHelResponse);
			COFIBER_AWAIT send_resp.future();
			HEL_CHECK(send_resp.error());
			
			helix::SendDescriptor<M> send_lane(helix::Dispatcher::global(), lane,
					descriptor, 0, 0, kHelResponse);
			COFIBER_AWAIT send_lane.future();
			HEL_CHECK(send_lane.error());
		}else{
			throw std::runtime_error("Unexpected request type");
		}
	}
}))

COFIBER_ROUTINE(cofiber::future<Entity>, Entity::createObject(std::string name,
		const Properties &properties,
		std::function<cofiber::future<helix::UniqueDescriptor>(AnyQuery)> handler) const, ([=] {
	managarm::mbus::CntRequest req;
	req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
	req.set_parent_id(_id);

	for(auto kv : properties) {
		managarm::mbus::Descriptor literal;
		literal.set_string(kv.second);
		req.mutable_descriptor()->mutable_fields()->insert({ kv.first, literal });
	}

	auto serialized = req.SerializeAsString();
	helix::SendString<M> send_req(_connection->dispatcher, _connection->pipe,
			serialized.data(), serialized.size(), 0, 0, kHelRequest);
	COFIBER_AWAIT send_req.future();
	HEL_CHECK(send_req.error());

	// recevie and parse the response.
	uint8_t buffer[128];
	helix::RecvString<M> recv_resp(_connection->dispatcher, _connection->pipe,
			buffer, 128, 0, 0, kHelResponse);
	COFIBER_AWAIT recv_resp.future();
	HEL_CHECK(recv_resp.error());

	managarm::mbus::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);
	
	helix::RecvDescriptor<M> recv_lane(_connection->dispatcher, _connection->pipe,
			0, 0, kHelResponse);
	COFIBER_AWAIT recv_lane.future();
	HEL_CHECK(recv_lane.error());
	handleObject(_connection, handler, helix::UniquePipe(recv_lane.descriptor()));

	COFIBER_RETURN(Entity(_connection, resp.id()));
}))

COFIBER_ROUTINE(cofiber::no_future, handleObserver(std::shared_ptr<Connection> connection,
		std::function<void(AnyEvent)> handler, helix::UniquePipe p),
		([connection, handler, lane = std::move(p)] {
	using M = helix::AwaitMechanism;

	while(true) {
		char buffer[128];
		helix::RecvString<M> recv_req(helix::Dispatcher::global(), lane,
				buffer, 128, 0, 0, kHelRequest);
		COFIBER_AWAIT recv_req.future();
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

COFIBER_ROUTINE(cofiber::future<Observer>, Entity::linkObserver(const AnyFilter &filter,
		std::function<void(AnyEvent)> handler) const, ([=] {
	managarm::mbus::CntRequest req;
	req.set_req_type(managarm::mbus::CntReqType::LINK_OBSERVER);
	req.set_id(_id);

	if(filter.type() == typeid(EqualsFilter)) {
		auto real = boost::get<EqualsFilter>(filter);
		auto msg = req.mutable_filter()->mutable_equals_filter();
		msg->set_path(real.getPath());
		msg->set_value(real.getValue());
	}else{
		throw std::runtime_error("Unexpected filter type");
	}

	auto serialized = req.SerializeAsString();
	helix::SendString<M> send_req(_connection->dispatcher, _connection->pipe,
			serialized.data(), serialized.size(), 0, 0, kHelRequest);
	COFIBER_AWAIT send_req.future();
	HEL_CHECK(send_req.error());

	// recevie and parse the response.
	uint8_t buffer[128];
	helix::RecvString<M> recv_resp(_connection->dispatcher, _connection->pipe,
			buffer, 128, 0, 0, kHelResponse);
	COFIBER_AWAIT recv_resp.future();
	HEL_CHECK(recv_resp.error());

	managarm::mbus::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);
	
	helix::RecvDescriptor<M> recv_lane(_connection->dispatcher, _connection->pipe,
			0, 0, kHelResponse);
	COFIBER_AWAIT recv_lane.future();
	HEL_CHECK(recv_lane.error());
	handleObserver(_connection, handler, helix::UniquePipe(recv_lane.descriptor()));

	COFIBER_RETURN(Observer());
}))

COFIBER_ROUTINE(cofiber::future<helix::UniqueDescriptor>, Entity::bind() const, ([=] {
	managarm::mbus::CntRequest req;
	req.set_req_type(managarm::mbus::CntReqType::BIND2);
	req.set_id(_id);

	auto serialized = req.SerializeAsString();
	helix::SendString<M> send_req(_connection->dispatcher, _connection->pipe,
			serialized.data(), serialized.size(), 0, 0, kHelRequest);
	COFIBER_AWAIT send_req.future();
	HEL_CHECK(send_req.error());

	// recevie and parse the response.
	uint8_t buffer[128];
	helix::RecvString<M> recv_resp(_connection->dispatcher, _connection->pipe,
			buffer, 128, 0, 0, kHelResponse);
	COFIBER_AWAIT recv_resp.future();
	HEL_CHECK(recv_resp.error());

	managarm::mbus::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);
	
	helix::RecvDescriptor<M> recv_desc(_connection->dispatcher, _connection->pipe,
			0, 0, kHelResponse);
	COFIBER_AWAIT recv_desc.future();
	HEL_CHECK(recv_desc.error());
	
	COFIBER_RETURN(recv_desc.descriptor());
}))

} } // namespace mbus::_detail

