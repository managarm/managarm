
#include <frigg/callback.hpp>
#include <frigg/smart_ptr.hpp>

#include <helx.hpp>

#include <frigg/glue-hel.hpp>
#include <frigg/protobuf.hpp>
#include <frigg/string.hpp>
#include <frigg/vector.hpp>
#include <frigg/hashmap.hpp>

#include "mbus.frigg_pb.hpp"

helx::EventHub eventHub = helx::EventHub::create();

struct Capability {
	Capability();

	frigg::String<Allocator> name;
};

Capability::Capability()
: name(*allocator) { }

int64_t nextObjectId = 1;

struct Object {
	Object(int64_t object_id);

	const int64_t objectId;

	frigg::Vector<Capability, Allocator> caps;
};

Object::Object(int64_t object_id)
: objectId(object_id), caps(*allocator) { }

frigg::Hashmap<int64_t, frigg::SharedPtr<Object>, frigg::DefaultHasher<int64_t>, Allocator>
allObjects(frigg::DefaultHasher<int64_t>(), *(allocator.unsafeGet()));

struct Connection {
	Connection(helx::Pipe pipe);

	helx::Pipe pipe;
};

Connection::Connection(helx::Pipe pipe)
: pipe(frigg::move(pipe)) { }

frigg::LazyInitializer<frigg::Vector<frigg::WeakPtr<Connection>, Allocator>> allConnections;

void broadcastRegister(frigg::SharedPtr<Object> object) {
	for(size_t i = 0; i < allConnections->size(); i++) {
		frigg::SharedPtr<Connection> other((*allConnections)[i]);
		if(!other)
			continue;

		managarm::mbus::SvrRequest<Allocator> request(*allocator);
		
		for(size_t i = 0; i < object->caps.size(); i++) {
			managarm::mbus::Capability<Allocator> capability(*allocator);
			capability.set_name(object->caps[i].name);
			request.add_caps(frigg::move(capability));
		}

		frigg::String<Allocator> serialized(*allocator);
		request.SerializeToString(&serialized);
		other->pipe.sendString(serialized.data(), serialized.size(), 0, 0);
	}
}

// --------------------------------------------------------
// RequestClosure
// --------------------------------------------------------

struct RequestClosure : frigg::BaseClosure<RequestClosure> {
public:
	RequestClosure(frigg::SharedPtr<Connection> object);

	void operator() ();

private:
	void recvRequest(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	frigg::SharedPtr<Connection> connection;
	uint8_t buffer[128];
};

RequestClosure::RequestClosure(frigg::SharedPtr<Connection> connection)
: connection(frigg::move(connection)) { }

void RequestClosure::operator() () {
	auto callback = CALLBACK_MEMBER(this, &RequestClosure::recvRequest);
	HEL_CHECK(connection->pipe.recvString(buffer, 128, eventHub, kHelAnyRequest, 0,
			callback.getObject(), callback.getFunction()));
}

void RequestClosure::recvRequest(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	if(error == kHelErrPipeClosed) {
		suicide(*allocator);
		return;
	}
	HEL_CHECK(error);
	
	managarm::mbus::CntRequest<Allocator> request(*allocator);
	request.ParseFromArray(buffer, length);
	
	switch(request.req_type()) {
	case managarm::mbus::CntReqType::REGISTER: {
		auto object = frigg::makeShared<Object>(*allocator, nextObjectId++);
		allObjects.insert(object->objectId, object);
		
		for(size_t i = 0; i < request.caps_size(); i++) {
			Capability capability;
			capability.name = request.caps(i).name();
			object->caps.push(frigg::move(capability));
		}

		broadcastRegister(frigg::move(object));
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
	auto callback = CALLBACK_MEMBER(this, &AcceptClosure::accepted);
	p_server.accept(eventHub, callback.getObject(), callback.getFunction());
}

void AcceptClosure::accepted(HelError error, HelHandle handle) {
	HEL_CHECK(error);
	infoLogger->log() << "Connected" << frigg::EndLog();
	
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
	HEL_CHECK(helSendDescriptor(parent_handle, client.getHandle(), 0, 0));
	HEL_CHECK(helCloseDescriptor(parent_handle));
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

