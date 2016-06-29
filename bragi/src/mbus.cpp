
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include <bragi/mbus.hpp>
#include <libchain/all.hpp>
#include <mbus.pb.h>

namespace bragi_mbus {

// --------------------------------------------------------
// Connection
// --------------------------------------------------------

Connection::Connection(helx::EventHub &event_hub)
: eventHub(event_hub), objectHandler(nullptr) { }

void Connection::setObjectHandler(ObjectHandler *handler) {
	objectHandler = handler;
}

void Connection::connect(frigg::CallbackPtr<void()> callback) {
	auto closure = new ConnectClosure(*this, callback);
	(*closure)();
}

void Connection::registerObject(std::string capability,
		frigg::CallbackPtr<void(ObjectId)> callback) {
	auto closure = new RegisterClosure(*this, capability, callback);
	(*closure)();
}

void Connection::enumerate(std::initializer_list<std::string> capabilities,
		frigg::CallbackPtr<void(std::vector<ObjectId>)> callback) {
	enumerate(std::vector<std::string>(capabilities), callback);
}

void Connection::enumerate(std::vector<std::string> capabilities,
		frigg::CallbackPtr<void(std::vector<ObjectId>)> callback) {
	auto closure = new EnumerateClosure(*this, std::move(capabilities), callback);
	(*closure)();
}

void Connection::queryIf(ObjectId object_id, frigg::CallbackPtr<void(HelHandle)> callback) {
	auto closure = new QueryIfClosure(*this, object_id, callback);
	(*closure)();
}

// --------------------------------------------------------
// Connection::ConnectClosure
// --------------------------------------------------------

Connection::ConnectClosure::ConnectClosure(Connection &connection,
		frigg::CallbackPtr<void()> on_connect)
: connection(connection), onConnect(on_connect) { }

void Connection::ConnectClosure::operator() () {
	const char *mbus_path = "config/mbus";
	HelHandle mbus_handle;
	HEL_CHECK(helRdOpen(mbus_path, strlen(mbus_path), &mbus_handle));
	
	helx::Client mbus_connect(mbus_handle);
	mbus_connect.connect(connection.eventHub, CALLBACK_MEMBER(this, &ConnectClosure::connected));
}

void Connection::ConnectClosure::connected(HelError error, HelHandle handle) {
	HEL_CHECK(error);
	connection.mbusPipe = helx::Pipe(handle);
	onConnect();

	processRequest();
}

void Connection::ConnectClosure::processRequest() {
	HEL_CHECK(connection.mbusPipe.recvStringReq(buffer, 128,
			connection.eventHub, kHelAnyRequest, 0,
			CALLBACK_MEMBER(this, &ConnectClosure::recvdRequest)));
}

void Connection::ConnectClosure::recvdRequest(HelError error,
		int64_t msg_request, int64_t msg_seq, size_t length) {
	managarm::mbus::SvrRequest request;
	request.ParseFromArray(buffer, length);

	if(request.req_type() == managarm::mbus::SvrReqType::BROADCAST) {
		// do nothing for now
	}else if(request.req_type() == managarm::mbus::SvrReqType::REQUIRE_IF) {
		auto closure = new RequireIfClosure(connection, msg_request, request.object_id());
		(*closure)();
	}else{
		assert(!"Unexpected request type");
	}

	processRequest();
}

// --------------------------------------------------------
// Connection::RegisterClosure
// --------------------------------------------------------

Connection::RegisterClosure::RegisterClosure(Connection &connection, std::string capability,
		frigg::CallbackPtr<void(ObjectId)> callback)
: connection(connection), capability(capability), callback(callback) { }

void Connection::RegisterClosure::operator() () {
	managarm::mbus::CntRequest request;
	request.set_req_type(managarm::mbus::CntReqType::REGISTER);

	managarm::mbus::Capability *cap = request.add_caps();
	cap->set_name(capability);

	std::string serialized;
	request.SerializeToString(&serialized);
	connection.mbusPipe.sendStringReq(serialized.data(), serialized.size(), 1, 0);
	
	HEL_CHECK(connection.mbusPipe.recvStringResp(buffer, 128, connection.eventHub, 1, 0,
			CALLBACK_MEMBER(this, &RegisterClosure::recvdResponse)));
}

void Connection::RegisterClosure::recvdResponse(HelError error,
		int64_t msg_request, int64_t msg_sequence, size_t length) {
	HEL_CHECK(error);

	managarm::mbus::SvrResponse response;
	response.ParseFromArray(buffer, length);
	
	callback(response.object_id());
	delete this;
}

// --------------------------------------------------------
// Connection::EnumerateClosure
// --------------------------------------------------------

Connection::EnumerateClosure::EnumerateClosure(Connection &connection,
		std::vector<std::string> capabilities,
		frigg::CallbackPtr<void(std::vector<ObjectId>)> callback)
: connection(connection), capabilities(std::move(capabilities)), callback(callback) { }

void Connection::EnumerateClosure::operator() () {
	auto action = libchain::compose([=] (std::string *serialized) {
		managarm::mbus::CntRequest request;
		request.set_req_type(managarm::mbus::CntReqType::ENUMERATE);

		for(auto it = capabilities.begin(); it != capabilities.end(); ++it) {
			managarm::mbus::Capability *cap = request.add_caps();
			cap->set_name(*it);
		}

		request.SerializeToString(serialized);
		return connection.mbusPipe.sendStringReq(serialized->data(), serialized->size(),
				connection.eventHub, 1, 0)
		+ libchain::apply([=] (HelError error) { HEL_CHECK(error); })
		+ libchain::apply([=] () {
			HEL_CHECK(connection.mbusPipe.recvStringResp(buffer, 128, connection.eventHub, 1, 0,
					CALLBACK_MEMBER(this, &EnumerateClosure::recvdResponse)));
		});
	}, std::string());

	libchain::run(std::move(action));
}

void Connection::EnumerateClosure::recvdResponse(HelError error,
		int64_t msg_request, int64_t msg_sequence, size_t length) {
	HEL_CHECK(error);

	managarm::mbus::SvrResponse response;
	response.ParseFromArray(buffer, length);
	
	std::vector<ObjectId> result;
	result.push_back(response.object_id());
	callback(result);
	delete this;
}

// --------------------------------------------------------
// Connection::QueryIfClosure
// --------------------------------------------------------

Connection::QueryIfClosure::QueryIfClosure(Connection &connection, ObjectId object_id,
		frigg::CallbackPtr<void(HelHandle)> callback)
: connection(connection), objectId(object_id), callback(callback) { }

void Connection::QueryIfClosure::operator() () {
	auto action = libchain::compose([=] (std::string *serialized) {
		managarm::mbus::CntRequest request;
		request.set_req_type(managarm::mbus::CntReqType::QUERY_IF);
		request.set_object_id(objectId);
		request.SerializeToString(serialized);
		
		return connection.mbusPipe.sendStringReq(serialized->data(), serialized->size(),
				connection.eventHub, 1, 0)
		+ libchain::apply([=] (HelError error) { HEL_CHECK(error); });
	}, std::string())
	+ libchain::apply([=] () {
		connection.mbusPipe.recvDescriptorResp(connection.eventHub, 1, 1,
				CALLBACK_MEMBER(this, &QueryIfClosure::recvdDescriptor));
	});

	libchain::run(std::move(action));
}

void Connection::QueryIfClosure::recvdDescriptor(HelError error,
		int64_t msg_request, int64_t msg_sequence, HelHandle handle) {
	HEL_CHECK(error);
	callback(handle);
	delete this;
}

// --------------------------------------------------------
// Connection::RequireIfClosure
// --------------------------------------------------------

Connection::RequireIfClosure::RequireIfClosure(Connection &connection, int64_t request_id,
		ObjectId object_id)
: connection(connection), requestId(request_id), objectId(object_id) { }

void Connection::RequireIfClosure::operator() () {
	assert(connection.objectHandler);
	connection.objectHandler->requireIf(objectId,
			CALLBACK_MEMBER(this, &RequireIfClosure::requiredIf));
}

void Connection::RequireIfClosure::requiredIf(HelHandle handle) {
	printf("[bragi/src/mbus] Connection:RequireIfClosure sendDescriptorResp\n");
	auto action = connection.mbusPipe.sendDescriptorResp(handle, connection.eventHub, requestId, 1)
			+ libchain::apply([=] (HelError error) { HEL_CHECK(error); });
	libchain::run(std::move(action));
	delete this;
}

} // bragi_mbus

