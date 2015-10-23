
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include <bragi/mbus.hpp>
#include <mbus.pb.h>

namespace bragi_mbus {

// --------------------------------------------------------
// Connection
// --------------------------------------------------------

Connection::Connection(helx::EventHub &event_hub)
: eventHub(event_hub) { }

void Connection::connect(frigg::CallbackPtr<void()> callback) {
	auto closure = new ConnectClosure(*this, callback);
	(*closure)();
}

void Connection::enumerate(std::string capability,
		frigg::CallbackPtr<void(std::vector<ObjectId>)> callback) {
	auto closure = new EnumerateClosure(*this, capability, callback);
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
		frigg::CallbackPtr<void()> callback)
: connection(connection), callback(callback) { }

void Connection::ConnectClosure::operator() () {
	const char *mbus_path = "config/mbus";
	HelHandle mbus_handle;
	HEL_CHECK(helRdOpen(mbus_path, strlen(mbus_path), &mbus_handle));
	
	helx::Client mbus_connect(mbus_handle);
	auto callback = CALLBACK_MEMBER(this, &ConnectClosure::connected);
	mbus_connect.connect(connection.eventHub,
			callback.getObject(), callback.getFunction());
}

void Connection::ConnectClosure::connected(HelError error, HelHandle handle) {
	HEL_CHECK(error);
	connection.mbusPipe = helx::Pipe(handle);
	callback();
	delete this;
}

// --------------------------------------------------------
// Connection::EnumerateClosure
// --------------------------------------------------------

Connection::EnumerateClosure::EnumerateClosure(Connection &connection, std::string capability,
		frigg::CallbackPtr<void(std::vector<ObjectId>)> callback)
: connection(connection), capability(capability), callback(callback) { }

void Connection::EnumerateClosure::operator() () {
	managarm::mbus::CntRequest request;
	request.set_req_type(managarm::mbus::CntReqType::ENUMERATE);

	managarm::mbus::Capability *cap = request.add_caps();
	cap->set_name(capability);

	std::string serialized;
	request.SerializeToString(&serialized);
	connection.mbusPipe.sendStringReq(serialized.data(), serialized.size(), 1, 0);
	
	auto callback = CALLBACK_MEMBER(this, &EnumerateClosure::recvdResponse);
	connection.mbusPipe.recvStringResp(buffer, 128, connection.eventHub, 1, 0,
			callback.getObject(), callback.getFunction());
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
	managarm::mbus::CntRequest request;
	request.set_req_type(managarm::mbus::CntReqType::QUERY_IF);
	request.set_object_id(objectId);

	std::string serialized;
	request.SerializeToString(&serialized);
	connection.mbusPipe.sendStringReq(serialized.data(), serialized.size(), 1, 0);

	auto callback = CALLBACK_MEMBER(this, &QueryIfClosure::recvdDescriptor);
	connection.mbusPipe.recvDescriptorResp(connection.eventHub, 1, 1,
			callback.getObject(), callback.getFunction());
}

void Connection::QueryIfClosure::recvdDescriptor(HelError error,
		int64_t msg_request, int64_t msg_sequence, HelHandle handle) {
	HEL_CHECK(error);
	callback(handle);
	delete this;
}

} // bragi_mbus

