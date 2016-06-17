
#include <frigg/callback.hpp>
#include <frigg/smart_ptr.hpp>

#include <frigg/glue-hel.hpp>
#include <frigg/protobuf.hpp>
#include <frigg/string.hpp>
#include <frigg/vector.hpp>
#include <frigg/hashmap.hpp>
#include <frigg/chain-all.hpp>

#include <helx.hpp>

#include <mbus.frigg_pb.hpp>

helx::EventHub eventHub = helx::EventHub::create();

// --------------------------------------------------------
// Capability
// --------------------------------------------------------

struct Capability {
	Capability();

	frigg::String<Allocator> name;
};

Capability::Capability()
: name(*allocator) { }

// --------------------------------------------------------
// Object
// --------------------------------------------------------

class Connection;

struct Object {
	Object(int64_t object_id);

	bool hasCapability(frigg::StringView name);

	const int64_t objectId;
	frigg::SharedPtr<Connection> connection;

	frigg::Vector<Capability, Allocator> caps;
};

Object::Object(int64_t object_id)
: objectId(object_id), caps(*allocator) { }

bool Object::hasCapability(frigg::StringView name) {
	for(size_t i = 0; i < caps.size(); i++)
		if(caps[i].name == name)
			return true;
	
	return false;
}

frigg::Hashmap<int64_t, frigg::SharedPtr<Object>, frigg::DefaultHasher<int64_t>, Allocator>
allObjects(frigg::DefaultHasher<int64_t>(), *(allocator.unsafeGet()));

int64_t nextObjectId = 1;

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
		frigg::SharedPtr<Connection> other((*allConnections)[i]);
		if(!other)
			continue;
		if(other.get() == object->connection.get())
			continue;

		managarm::mbus::SvrRequest<Allocator> request(*allocator);
		request.set_req_type(managarm::mbus::SvrReqType::BROADCAST);
		request.set_object_id(object->objectId);
		
		for(size_t i = 0; i < object->caps.size(); i++) {
			managarm::mbus::Capability<Allocator> capability(*allocator);
			capability.set_name(object->caps[i].name);
			request.add_caps(frigg::move(capability));
		}

		frigg::String<Allocator> serialized(*allocator);
		request.SerializeToString(&serialized);
		other->pipe.sendStringReq(serialized.data(), serialized.size(), 0, 0);
	}
}

// --------------------------------------------------------
// QueryIfClosure
// --------------------------------------------------------

struct QueryIfClosure : frigg::BaseClosure<QueryIfClosure> {
public:
	QueryIfClosure(frigg::SharedPtr<Connection> query_connection,
			frigg::SharedPtr<Object> object, int64_t query_request_id,
			int64_t require_request_id);

	void operator() ();

private:
	void recvdPipe(HelError error, int64_t msg_request, int64_t msg_seq, HelHandle handle);

	frigg::SharedPtr<Connection> queryConnection;
	frigg::SharedPtr<Object> object;
	int64_t queryRequestId;
	int64_t requireRequestId;
};

QueryIfClosure::QueryIfClosure(frigg::SharedPtr<Connection> connection,
		frigg::SharedPtr<Object> object, int64_t query_request_id, int64_t require_request_id)
: queryConnection(frigg::move(connection)), object(object),
		queryRequestId(query_request_id), requireRequestId(require_request_id) { }

void QueryIfClosure::operator() () {
	object->connection->pipe.recvDescriptorResp(eventHub, requireRequestId, 1,
			CALLBACK_MEMBER(this, &QueryIfClosure::recvdPipe));
}

void QueryIfClosure::recvdPipe(HelError error, int64_t msg_request, int64_t msg_seq,
		HelHandle handle) {
	queryConnection->pipe.sendDescriptorResp(handle, queryRequestId, 1);
	HEL_CHECK(helCloseDescriptor(handle));

	suicide(*allocator);
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
	if(error == kHelErrPipeClosed) {
		infoLogger->log() << "wtf" << frigg::EndLog();
		suicide(*allocator);
		return;
	}
	HEL_CHECK(error);
	
	managarm::mbus::CntRequest<Allocator> recvd_request(*allocator);
	recvd_request.ParseFromArray(buffer, length);
	
	switch(recvd_request.req_type()) {
	case managarm::mbus::CntReqType::REGISTER: {
		auto action = frigg::compose([=] (auto request, auto serialized) {
			return frigg::apply([=] () {
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
			})
			+ frigg::await<void(HelError)>([=] (auto callback) {
				connection->pipe.sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0, callback);
			})
			+ frigg::apply([=] (HelError error) {
				HEL_CHECK(error);
			});
		}, frigg::move(recvd_request), frigg::String<Allocator>(*allocator));

		frigg::run(frigg::move(action), allocator.get());
		/*broadcastRegister(frigg::move(object));
		*/
	} break;
	case managarm::mbus::CntReqType::ENUMERATE: {
		for(auto it = allObjects.iterator(); it; ++it) {
			frigg::UnsafePtr<Object> object = it->get<1>();
			
			bool matching = true;
			for(size_t i = 0; i < recvd_request.caps_size(); i++) {
				if(!object->hasCapability(recvd_request.caps(i).name()))
					matching = false;
			}
			if(!matching)
				continue;

			managarm::mbus::SvrResponse<Allocator> response(*allocator);
			response.set_object_id(object->objectId);

			frigg::String<Allocator> serialized(*allocator);
			response.SerializeToString(&serialized);
			connection->pipe.sendStringResp(serialized.data(), serialized.size(),
					msg_request, 0);

			(*this)(); //FIXME
			return;
		}

		assert(!"No matching object");
	} break;
	case managarm::mbus::CntReqType::QUERY_IF: {
		frigg::SharedPtr<Object> *object = allObjects.get(recvd_request.object_id());
		assert(object);

		managarm::mbus::SvrRequest<Allocator> require_request(*allocator);
		require_request.set_req_type(managarm::mbus::SvrReqType::REQUIRE_IF);
		require_request.set_object_id(recvd_request.object_id());

		int64_t require_request_id = (*object)->connection->nextRequestId++;
		frigg::String<Allocator> serialized(*allocator);
		require_request.SerializeToString(&serialized);
		(*object)->connection->pipe.sendStringReq(serialized.data(), serialized.size(),
				require_request_id, 0);
	
		frigg::runClosure<QueryIfClosure>(*allocator, connection,
				*object, msg_request, require_request_id);
	} break;
	default:
		assert(!"Illegal request type");
	};

	(*this)();
}

// --------------------------------------------------------
// AcceptClosure
// --------------------------------------------------------

struct AcceptClosure : frigg::BaseClosure<AcceptClosure> {
public:
	AcceptClosure(helx::Server server);

	void operator() ();

private:
	void accepted(HelError error, HelHandle handle);

	helx::Server p_server;
};

AcceptClosure::AcceptClosure(helx::Server server)
: p_server(frigg::move(server)) { }

void AcceptClosure::operator() () {
	p_server.accept(eventHub, CALLBACK_MEMBER(this, &AcceptClosure::accepted));
}

void AcceptClosure::accepted(HelError error, HelHandle handle) {
	HEL_CHECK(error);
	
	auto connection = frigg::makeShared<Connection>(*allocator, helx::Pipe(handle));
	allConnections->push(frigg::WeakPtr<Connection>(connection));
	frigg::runClosure<RequestClosure>(*allocator, frigg::move(connection));
	(*this)();
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

typedef void (*InitFuncPtr) ();
extern InitFuncPtr __init_array_start[];
extern InitFuncPtr __init_array_end[];

int main() {
	// we're using no libc, so we have to run constructors manually
	size_t init_count = __init_array_end - __init_array_start;
	for(size_t i = 0; i < init_count; i++)
		__init_array_start[i]();

	infoLogger.initialize(infoSink);
	infoLogger->log() << "Entering mbus" << frigg::EndLog();
	allocator.initialize(virtualAlloc);
	allConnections.initialize(*allocator);

	// start our server
	helx::Server server;
	helx::Client client;
	helx::Server::createServer(server, client);
	frigg::runClosure<AcceptClosure>(*allocator, frigg::move(server));

	const char *parent_path = "local/parent";
	HelHandle parent_handle;
	HEL_CHECK(helRdOpen(parent_path, strlen(parent_path), &parent_handle));

	helx::Pipe parent_pipe(parent_handle);
	HelError send_error;
	parent_pipe.sendDescriptorSync(client.getHandle(), eventHub,
			0, 0, kHelRequest, send_error);
	HEL_CHECK(send_error);
	parent_pipe.reset();
	client.reset();

	while(true)
		eventHub.defaultProcessEvents();

	HEL_CHECK(helExitThisThread());
	__builtin_unreachable();
}

asm ( ".global _start\n"
		"_start:\n"
		"\tcall main\n"
		"\tud2" );

extern "C"
int __cxa_atexit(void (*func) (void *), void *arg, void *dso_handle) {
	return 0;
}

void *__dso_handle;

