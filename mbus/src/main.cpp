
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/auxv.h>
#include <algorithm>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <vector>

#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>

#include "mbus.pb.h"

helix::Dispatcher dispatcher(helix::createHub());

// --------------------------------------------------------
// Capability
// --------------------------------------------------------

struct Capability {
	std::string name;
};

// --------------------------------------------------------
// Object
// --------------------------------------------------------

class Connection;

struct Object {
	Object(int64_t object_id);

	bool hasCapability(std::string name);

	const int64_t objectId;
	std::shared_ptr<Connection> connection;

	std::vector<Capability> caps;
};

Object::Object(int64_t object_id)
: objectId(object_id) { }

bool Object::hasCapability(std::string name) {
	return std::find_if(caps.begin(), caps.end(), [&] (const Capability &cap) {
		return cap.name == name;
	}) != caps.end();
}

std::unordered_map<int64_t, Object> allObjects;
int64_t nextObjectId = 1;

/*
// --------------------------------------------------------
// Connection
// --------------------------------------------------------

struct Connection {
	Connection(helx::Pipe pipe);

	helx::Pipe pipe;
	int64_t nextRequestId;
};

Connection::Connection(helx::Pipe pipe)
: pipe(frigg::move(pipe)), nextRequestId(0) { }

frigg::LazyInitializer<frigg::Vector<frigg::WeakPtr<Connection>, Allocator>> allConnections;

void broadcastRegister(frigg::SharedPtr<Object> object) {
	for(size_t i = 0; i < allConnections->size(); i++) {
		frigg::SharedPtr<Connection> other = (*allConnections)[i].grab();
		if(!other)
			continue;
		if(other.get() == object->connection.get())
			continue;

		auto action = frigg::compose([=] (frigg::String<Allocator> *serialized) {
			managarm::mbus::SvrRequest<Allocator> request(*allocator);
			request.set_req_type(managarm::mbus::SvrReqType::BROADCAST);
			request.set_object_id(object->objectId);
			
			for(size_t i = 0; i < object->caps.size(); i++) {
				managarm::mbus::Capability<Allocator> capability(*allocator);
				capability.set_name(object->caps[i].name);
				request.add_caps(frigg::move(capability));
			}
			
			request.SerializeToString(serialized);

			return other->pipe.sendStringReq(serialized->data(), serialized->size(), eventHub, 0, 0)
			+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
		}, frigg::String<Allocator>(*allocator));

		frigg::run(frigg::move(action), allocator.get());
	}
}

// --------------------------------------------------------
// RequestClosure
// --------------------------------------------------------

struct RequestClosure : frigg::BaseClosure<RequestClosure> {
public:
	RequestClosure(frigg::SharedPtr<Connection> connection);

	void operator() ();

private:
	void recvdRequest(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	frigg::SharedPtr<Connection> connection;
	uint8_t buffer[128];
};

RequestClosure::RequestClosure(frigg::SharedPtr<Connection> connection)
: connection(frigg::move(connection)) { }

void RequestClosure::operator() () {
	HEL_CHECK(connection->pipe.recvStringReq(buffer, 128, eventHub, kHelAnyRequest, 0,
			CALLBACK_MEMBER(this, &RequestClosure::recvdRequest)));
}

void RequestClosure::recvdRequest(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	if(error == kHelErrClosedRemotely) {
		suicide(*allocator);
		return;
	}
	HEL_CHECK(error);
	
	managarm::mbus::CntRequest<Allocator> recvd_request(*allocator);
	recvd_request.ParseFromArray(buffer, length);
	
	switch(recvd_request.req_type()) {
	case managarm::mbus::CntReqType::REGISTER: {
		
		auto action = frigg::compose([=] (auto request, auto serialized) {
			auto object = frigg::makeShared<Object>(*allocator, nextObjectId++);
			object->connection = frigg::SharedPtr<Connection>(connection);
			allObjects.insert(object->objectId, object);
			
			for(size_t i = 0; i < request->caps_size(); i++) {
				Capability capability;
				capability.name = request->caps(i).name();
				object->caps.push(frigg::move(capability));
			}

			managarm::mbus::SvrResponse<Allocator> response(*allocator);
			response.set_object_id(object->objectId);
			response.SerializeToString(serialized);
			
			return connection->pipe.sendStringResp(serialized->data(), serialized->size(),
					eventHub, msg_request, 0)
			+ frigg::lift([=] (HelError error) {
				HEL_CHECK(error);
				broadcastRegister(object);
			});
		}, frigg::move(recvd_request), frigg::String<Allocator>(*allocator));

		frigg::run(frigg::move(action), allocator.get());
	} break;
	case managarm::mbus::CntReqType::ENUMERATE: {
		frigg::SharedPtr<Object> found;
		for(auto it = allObjects.iterator(); it; ++it) {
			frigg::UnsafePtr<Object> object = it->get<1>();
			
			bool matching = true;
			for(size_t i = 0; i < recvd_request.caps_size(); i++) {
				if(!object->hasCapability(recvd_request.caps(i).name()))
					matching = false;
			}
			if(matching) {
				found = object.toShared();
				break;
			}
		}
		assert(found);

		auto action = frigg::compose([=]  (auto serialized) {
			managarm::mbus::SvrResponse<Allocator> response(*allocator);
			response.set_object_id(found->objectId);
			response.SerializeToString(serialized);
			
			return connection->pipe.sendStringResp(serialized->data(), serialized->size(),
					eventHub, msg_request, 0)
			+ frigg::lift([=] (HelError error) {
				HEL_CHECK(error);
			});
		}, frigg::String<Allocator>(*allocator));

		frigg::run(frigg::move(action), allocator.get());
	} break;
	case managarm::mbus::CntReqType::QUERY_IF: {
		frigg::SharedPtr<Object> *object = allObjects.get(recvd_request.object_id());
		assert(object);

		auto action = frigg::compose([=] (auto request) {
			int64_t require_request_id = (*object)->connection->nextRequestId++;
			
			return frigg::compose([=] (auto serialized) {
				managarm::mbus::SvrRequest<Allocator> require_request(*allocator);
				require_request.set_req_type(managarm::mbus::SvrReqType::REQUIRE_IF);
				require_request.set_object_id(request->object_id());
				require_request.SerializeToString(serialized);
			
				return (*object)->connection->pipe.sendStringReq(serialized->data(), serialized->size(),
							eventHub, require_request_id, 0)
				+ frigg::lift([=] (HelError error) { 
					HEL_CHECK(error); 
				});
			}, frigg::String<Allocator>(*allocator))
			+ frigg::await<void(HelError, int64_t, int64_t, HelHandle)>([=] (auto callback) {
				(*object)->connection->pipe.recvDescriptorResp(eventHub, require_request_id, 1,
						callback);
			})
			+ frigg::compose([=] (HelError error, int64_t require_msg_request,
					int64_t require_msg_seq, HelHandle handle) {
				HEL_CHECK(error);
				
				return connection->pipe.sendDescriptorResp(handle, eventHub, msg_request, 1)
				+ frigg::lift([=] (HelError error) {
					HEL_CHECK(error);
					HEL_CHECK(helCloseDescriptor(handle));
				});
			});
		}, frigg::move(recvd_request));

		frigg::run(frigg::move(action), allocator.get());
	} break;
	default:
		assert(!"Illegal request type");
	};

	(*this)();
}*/

COFIBER_ROUTINE(cofiber::no_future, serve(helix::UniquePipe p),
		[pipe = std::move(p)] () {
	using M = helix::AwaitMechanism;

	while(true) {
		char req_buffer[128];
		helix::RecvString<M> recv_req(dispatcher, pipe, req_buffer, 128,
				kHelAnyRequest, 0, kHelRequest);
		COFIBER_AWAIT recv_req.future();

		assert(!"Fix this");

		//FIXME: actually parse the protocol.

/*		char data_buffer[128];
		helix::RecvString<M> recv_data(dispatcher, pipe, data_buffer, 128,
				recv_req.requestId(), 1, kHelRequest);
		COFIBER_AWAIT recv_data.future();

		helLog(data_buffer, recv_data.actualLength());

		// send the success response.
		// FIXME: send an actually valid answer.
		helix::SendString<M> send_resp(dispatcher, pipe, nullptr, 0,
				recv_req.requestId(), 0, kHelResponse);
		COFIBER_AWAIT send_resp.future();*/
	}
})

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	std::cout << "Entering mbus" << std::endl;
	
	unsigned long xpipe;
	if(peekauxval(AT_XPIPE, &xpipe))
		throw std::runtime_error("No AT_XPIPE specified");

	serve(helix::UniquePipe(xpipe));

	while(true)
		dispatcher();
}

