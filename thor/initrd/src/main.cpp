
#include <frigg/callback.hpp>
#include <frigg/smart_ptr.hpp>

#include <frigg/glue-hel.hpp>
#include <frigg/protobuf.hpp>
#include <frigg/string.hpp>
#include <frigg/vector.hpp>
#include <frigg/hashmap.hpp>

#include <helx.hpp>

#include <fs.frigg_pb.hpp>
#include <mbus.frigg_pb.hpp>

helx::EventHub eventHub = helx::EventHub::create();
helx::Pipe mbusPipe;

// --------------------------------------------------------
// OpenFile
// --------------------------------------------------------

struct OpenFile {
	OpenFile(char *image, size_t size);

	char *image;
	size_t size;
	uint64_t offset;
};

OpenFile::OpenFile(char *image, size_t size)
: image(image), size(size), offset(0) { }

// --------------------------------------------------------

struct Connection {
	Connection(helx::Pipe pipe);

	void operator() ();
	
	helx::Pipe &getPipe();

	int attachOpenFile(OpenFile *handle);
	OpenFile *getOpenFile(int handle);

private:
	void recvRequest(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	helx::Pipe pipe;

	frigg::Hashmap<int, OpenFile *,
			frigg::DefaultHasher<int>, Allocator> fileHandles;
	int nextHandle;
	uint8_t buffer[128];
};

struct StatClosure {
	StatClosure(Connection &connection, int64_t response_id,
			managarm::fs::CntRequest<Allocator> request);

	void operator() ();

	Connection &connection;
	int64_t responseId;
	managarm::fs::CntRequest<Allocator> request;
};

struct OpenClosure {
	OpenClosure(Connection &connection, int64_t response_id,
			managarm::fs::CntRequest<Allocator> request);

	void operator() ();

	Connection &connection;
	int64_t responseId;
	managarm::fs::CntRequest<Allocator> request;
};

struct ReadClosure {
	ReadClosure(Connection &connection, int64_t response_id,
			managarm::fs::CntRequest<Allocator> request);

	void operator() ();

	Connection &connection;
	int64_t responseId;
	managarm::fs::CntRequest<Allocator> request;
};

// --------------------------------------------------------
// Connection
// --------------------------------------------------------

Connection::Connection(helx::Pipe pipe)
: pipe(frigg::move(pipe)), fileHandles(frigg::DefaultHasher<int>(), *allocator), nextHandle(1) { }

void Connection::operator() () {
	HEL_CHECK(pipe.recvStringReq(buffer, 128, eventHub, kHelAnyRequest, 0,
			CALLBACK_MEMBER(this, &Connection::recvRequest)));
}

helx::Pipe &Connection::getPipe() {
	return pipe;
}

int Connection::attachOpenFile(OpenFile *file) {
	int handle = nextHandle++;
	fileHandles.insert(handle, file);
	return handle;
}

OpenFile *Connection::getOpenFile(int handle) {
	OpenFile **file = fileHandles.get(handle);
	assert(file);
	return *file;
}

void Connection::recvRequest(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	HEL_CHECK(error);

	managarm::fs::CntRequest<Allocator> request(*allocator);
	request.ParseFromArray(buffer, length);

	if(request.req_type() == managarm::fs::CntReqType::FSTAT) {
		auto closure = frigg::construct<StatClosure>(*allocator,
				*this, msg_request, frigg::move(request));
		(*closure)();
	}else if(request.req_type() == managarm::fs::CntReqType::OPEN) {
		auto closure = frigg::construct<OpenClosure>(*allocator,
				*this, msg_request, frigg::move(request));
		(*closure)();
	}else if(request.req_type() == managarm::fs::CntReqType::READ) {
		auto closure = frigg::construct<ReadClosure>(*allocator,
				*this, msg_request, frigg::move(request));
		(*closure)();
	}else{
		frigg::panicLogger.log() << "Illegal request type" << frigg::EndLog();
	}

	(*this)();
}

// --------------------------------------------------------
// StatClosure
// --------------------------------------------------------

StatClosure::StatClosure(Connection &connection, int64_t response_id,
		managarm::fs::CntRequest<Allocator> request)
: connection(connection), responseId(response_id), request(frigg::move(request)) { }

void StatClosure::operator() () {
	auto open_file = connection.getOpenFile(request.fd());
	managarm::fs::SvrResponse<Allocator> response(*allocator);
	response.set_error(managarm::fs::Errors::SUCCESS);
	response.set_file_size(open_file->size);

	frigg::String<Allocator> serialized(*allocator);
	response.SerializeToString(&serialized);
	connection.getPipe().sendStringResp(serialized.data(), serialized.size(), responseId, 0);
}

// --------------------------------------------------------
// OpenClosure
// --------------------------------------------------------

OpenClosure::OpenClosure(Connection &connection, int64_t response_id,
		managarm::fs::CntRequest<Allocator> request)
: connection(connection), responseId(response_id), request(frigg::move(request)) { }

void OpenClosure::operator() () {
	frigg::String<Allocator> full_path(*allocator, "initrd/");
	full_path += request.path();

	HelHandle image_handle;
	HelError image_error = helRdOpen(full_path.data(), full_path.size(), &image_handle);

	if(image_error == kHelErrNoSuchPath) {
		managarm::fs::SvrResponse<Allocator> response(*allocator);
		response.set_error(managarm::fs::Errors::FILE_NOT_FOUND);

		frigg::String<Allocator> serialized(*allocator);
		response.SerializeToString(&serialized);
		connection.getPipe().sendStringResp(serialized.data(), serialized.size(), responseId, 0);
		return;
	}
	HEL_CHECK(image_error);

	size_t image_size;
	void *image_ptr;
	HEL_CHECK(helMemoryInfo(image_handle, &image_size));
	HEL_CHECK(helMapMemory(image_handle, kHelNullHandle, nullptr, image_size,
			kHelMapReadOnly, &image_ptr));
	HEL_CHECK(helCloseDescriptor(image_handle));

	auto file = frigg::construct<OpenFile>(*allocator, (char *)image_ptr, image_size);
	int handle = connection.attachOpenFile(file);

	managarm::fs::SvrResponse<Allocator> response(*allocator);
	response.set_error(managarm::fs::Errors::SUCCESS);
	response.set_fd(handle);

	frigg::String<Allocator> serialized(*allocator);
	response.SerializeToString(&serialized);
	connection.getPipe().sendStringResp(serialized.data(), serialized.size(), responseId, 0);
}

// --------------------------------------------------------
// ReadClosure
// --------------------------------------------------------

ReadClosure::ReadClosure(Connection &connection, int64_t response_id,
		managarm::fs::CntRequest<Allocator> request)
: connection(connection), responseId(response_id), request(frigg::move(request)) { }

void ReadClosure::operator() () {
	auto open_file = connection.getOpenFile(request.fd());

	if(open_file->offset >= open_file->size) {
		managarm::fs::SvrResponse<Allocator> response(*allocator);
		response.set_error(managarm::fs::Errors::END_OF_FILE);

		frigg::String<Allocator> serialized(*allocator);
		response.SerializeToString(&serialized);
		connection.getPipe().sendStringResp(serialized.data(), serialized.size(), responseId, 0);
		return;
	}

	size_t read_size = request.size();
	if(read_size > open_file->size - open_file->offset)
		read_size = open_file->size - open_file->offset;

	managarm::fs::SvrResponse<Allocator> response(*allocator);
	response.set_error(managarm::fs::Errors::SUCCESS);
	response.set_buffer(frigg::String<Allocator>(*allocator,
			open_file->image + open_file->offset, read_size));

	frigg::String<Allocator> serialized(*allocator);
	response.SerializeToString(&serialized);
	connection.getPipe().sendStringResp(serialized.data(), serialized.size(), responseId, 0);

	open_file->offset += read_size;
}

// --------------------------------------------------------
// MbusClosure
// --------------------------------------------------------

struct MbusClosure {
	void operator() ();

private:
	void recvdRequest(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	uint8_t buffer[128];
};

void MbusClosure::operator() () {
	HEL_CHECK(mbusPipe.recvStringReq(buffer, 128, eventHub, kHelAnyRequest, 0,
			CALLBACK_MEMBER(this, &MbusClosure::recvdRequest)));
}

void MbusClosure::recvdRequest(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	managarm::mbus::SvrRequest<Allocator> request(*allocator);
	request.ParseFromArray(buffer, length);

	if(request.req_type() == managarm::mbus::SvrReqType::REQUIRE_IF) {
		helx::Pipe local, remote;
		helx::Pipe::createFullPipe(local, remote);
		mbusPipe.sendDescriptorResp(remote.getHandle(), msg_request, 1);
		remote.reset();

		auto connection = frigg::construct<Connection>(*allocator, frigg::move(local));
		(*connection)();
	}

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
	infoLogger->log() << "Entering initrd" << frigg::EndLog();
	allocator.initialize(virtualAlloc);
	
	// connect to mbus
	const char *mbus_path = "local/mbus";
	HelHandle mbus_handle;
	HEL_CHECK(helRdOpen(mbus_path, strlen(mbus_path), &mbus_handle));
	helx::Client mbus_client(mbus_handle);
	HelError mbus_error;
	mbus_client.connectSync(eventHub, mbus_error, mbusPipe);
	HEL_CHECK(mbus_error);
	mbus_client.reset();
	
	// register the initrd object
	managarm::mbus::CntRequest<Allocator> request(*allocator);
	request.set_req_type(managarm::mbus::CntReqType::REGISTER);
	
	managarm::mbus::Capability<Allocator> cap(*allocator);
	cap.set_name(frigg::String<Allocator>(*allocator, "initrd"));
	request.add_caps(frigg::move(cap));
	
	frigg::String<Allocator> serialized(*allocator);
	request.SerializeToString(&serialized);
	mbusPipe.sendStringReq(serialized.data(), serialized.size(), 123, 0);

	uint8_t buffer[128];
	HelError error;
	size_t length;
	mbusPipe.recvStringRespSync(buffer, 128, eventHub, 123, 0, error, length);
	HEL_CHECK(error);
	
	managarm::mbus::SvrResponse<Allocator> response(*allocator);
	response.ParseFromArray(buffer, length);
	
	frigg::runClosure<MbusClosure>(*allocator);

	// inform the parent that we are ready
	const char *parent_path = "local/parent";
	HelHandle parent_handle;
	HEL_CHECK(helRdOpen(parent_path, strlen(parent_path), &parent_handle));

	helx::Pipe parent_pipe(parent_handle);
	parent_pipe.sendStringReq(nullptr, 0, 0, 0);

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

