
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
	OpenFile(HelHandle file_memory, char *image, size_t size);

	HelHandle fileMemory;
	char *image;
	size_t size;
	uint64_t offset;
};

OpenFile::OpenFile(HelHandle file_memory, char *image, size_t size)
: fileMemory(file_memory), image(image), size(size), offset(0) { }

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

struct SeekClosure {
	SeekClosure(Connection &connection, int64_t response_id,
			managarm::fs::CntRequest<Allocator> request);

	void operator() ();

	Connection &connection;
	int64_t responseId;
	managarm::fs::CntRequest<Allocator> request;
};

struct MapClosure {
	MapClosure(Connection &connection, int64_t response_id,
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
	}else if(request.req_type() == managarm::fs::CntReqType::SEEK_ABS) {
		auto closure = frigg::construct<SeekClosure>(*allocator,
				*this, msg_request, frigg::move(request));
		(*closure)();
	}else if(request.req_type() == managarm::fs::CntReqType::MMAP) {
		auto closure = frigg::construct<MapClosure>(*allocator,
				*this, msg_request, frigg::move(request));
		(*closure)();
	}else{
		frigg::panicLogger() << "Illegal request type" << frigg::endLog;
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
	// FIXME: use chains instead of sync calls
	frigg::infoLogger() << "[thor/initrd/src/main] StatClosure:() sendStringResp" << frigg::endLog;
	auto action = connection.getPipe().sendStringResp(serialized.data(), serialized.size(),
			eventHub, responseId, 0)
	+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });		
	frigg::run(frigg::move(action), allocator.get()); 
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

	HelHandle image_memory;
	HelError image_error = helRdOpen(full_path.data(), full_path.size(), &image_memory);

	if(image_error == kHelErrNoSuchPath) {
		// FIXME: use chains instead of sync calls
		auto action = frigg::compose([=] (frigg::String<Allocator> *serialized) {
			managarm::fs::SvrResponse<Allocator> response(*allocator);
			response.set_error(managarm::fs::Errors::FILE_NOT_FOUND);
			response.SerializeToString(serialized);
			
			return connection.getPipe().sendStringResp(serialized->data(), serialized->size(),
					eventHub, responseId, 0)
			+ frigg::lift([=] (HelError error) { 
				HEL_CHECK(error); 
			});
		}, frigg::String<Allocator>(*allocator));
		frigg::run(frigg::move(action), allocator.get());
		
		return;
	}
	HEL_CHECK(image_error);

	size_t image_size;
	void *image_ptr;
	HEL_CHECK(helMemoryInfo(image_memory, &image_size));
	HEL_CHECK(helMapMemory(image_memory, kHelNullHandle, nullptr, 0, image_size,
			kHelMapReadOnly, &image_ptr));

	auto file = frigg::construct<OpenFile>(*allocator,
			image_memory, (char *)image_ptr, image_size);
	int handle = connection.attachOpenFile(file);

	managarm::fs::SvrResponse<Allocator> response(*allocator);
	response.set_error(managarm::fs::Errors::SUCCESS);
	response.set_fd(handle);
	response.set_file_type(managarm::fs::FileType::REGULAR);

	frigg::String<Allocator> serialized(*allocator);
	response.SerializeToString(&serialized);
	// FIXME: use chains instead of sync calls
	HelError send_open_error;
	connection.getPipe().sendStringRespSync(serialized.data(), serialized.size(), 
			eventHub, responseId, 0, send_open_error);
	HEL_CHECK(send_open_error);
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
		// FIXME: use chains instead of sync calls
		frigg::infoLogger() << "[thor/initrd/src/main] ReadClosure:() sendStringResp" << frigg::endLog;
		HelError send_read_error;
		connection.getPipe().sendStringRespSync(serialized.data(), serialized.size(),
				eventHub, responseId, 0, send_read_error);
		HEL_CHECK(send_read_error);
		return;
	}

	size_t read_size = request.size();
	if(read_size > open_file->size - open_file->offset)
		read_size = open_file->size - open_file->offset;
	
	auto action = frigg::compose([=] (frigg::String<Allocator> *serialized) {
		managarm::fs::SvrResponse<Allocator> response(*allocator);
		response.set_error(managarm::fs::Errors::SUCCESS);
		response.SerializeToString(serialized);
		
		return connection.getPipe().sendStringResp(serialized->data(), serialized->size(),
				eventHub, responseId, 0)
		+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
	}, frigg::String<Allocator>(*allocator))
	+ frigg::compose([=] () {
		char *ptr = open_file->image + open_file->offset;
		open_file->offset += read_size;

		return connection.getPipe().sendStringResp(ptr, read_size, eventHub, responseId, 1)
		+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
	});

	frigg::run(frigg::move(action), allocator.get());
}

// --------------------------------------------------------
// SeekClosure
// --------------------------------------------------------

SeekClosure::SeekClosure(Connection &connection, int64_t response_id,
		managarm::fs::CntRequest<Allocator> request)
: connection(connection), responseId(response_id), request(frigg::move(request)) { }

void SeekClosure::operator() () {
	auto open_file = connection.getOpenFile(request.fd());
	open_file->offset = request.rel_offset();

	managarm::fs::SvrResponse<Allocator> response(*allocator);
	response.set_error(managarm::fs::Errors::SUCCESS);
	response.set_offset(open_file->offset);

	frigg::String<Allocator> serialized(*allocator);
	response.SerializeToString(&serialized);
	// FIXME: use chains instead of sync calls
	HelError seek_error;
	connection.getPipe().sendStringRespSync(serialized.data(), serialized.size(),
			eventHub, responseId, 0, seek_error);
	HEL_CHECK(seek_error);
}

// --------------------------------------------------------
// MapClosure
// --------------------------------------------------------

MapClosure::MapClosure(Connection &connection, int64_t response_id,
		managarm::fs::CntRequest<Allocator> request)
: connection(connection), responseId(response_id), request(frigg::move(request)) { }

void MapClosure::operator() () {
	auto open_file = connection.getOpenFile(request.fd());

	// FIXME: use chains instead of sync calls
	auto action = frigg::compose([=] (frigg::String<Allocator> *resp_buffer) {
		managarm::fs::SvrResponse<Allocator> response(*allocator);
		response.set_error(managarm::fs::Errors::SUCCESS);
		response.SerializeToString(resp_buffer);
		
		return connection.getPipe().sendStringResp(resp_buffer->data(), resp_buffer->size(),
				eventHub, responseId, 0)
		+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
	}, frigg::String<Allocator>(*allocator))
	+ frigg::compose([=] () {
		return connection.getPipe().sendDescriptorResp(open_file->fileMemory, eventHub, responseId, 1)
		+ frigg::lift([=] (HelError error) { HEL_CHECK(error); });
	});

	frigg::run(frigg::move(action), allocator.get()); 
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
		
		//FIXME: this should not be sync
		HelError send_error;
		mbusPipe.sendDescriptorRespSync(remote.getHandle(), eventHub,
				msg_request, 1, send_error);
		HEL_CHECK(send_error);
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

	frigg::infoLogger() << "Entering initrd" << frigg::endLog;
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

	HelError register_error;
	frigg::String<Allocator> serialized(*allocator);
	request.SerializeToString(&serialized);
	mbusPipe.sendStringReqSync(serialized.data(), serialized.size(),
			eventHub, 123, 0, register_error);
	
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

	HelError ready_error;
	helx::Pipe parent_pipe(parent_handle);
	parent_pipe.sendStringReqSync(nullptr, 0, eventHub, 0, 0, ready_error);

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

