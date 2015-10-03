
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>
#include <frigg/libc.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/smart_ptr.hpp>
#include <frigg/string.hpp>
#include <frigg/vector.hpp>
#include <frigg/optional.hpp>
#include <frigg/tuple.hpp>
#include <frigg/hashmap.hpp>
#include <frigg/async2.hpp>
#include <frigg/protobuf.hpp>

#include <frigg/glue-hel.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include "ld-server.frigg_pb.hpp"
#include "posix.frigg_pb.hpp"

#include "common.hpp"
#include "device.hpp"
#include "vfs.hpp"
#include "process.hpp"
#include "dev_fs.hpp"

helx::EventHub eventHub = helx::EventHub::create();
helx::Client ldServerConnect;
helx::Pipe ldServerPipe;

struct LoadContext {
	LoadContext()
	: space(kHelNullHandle), response(*allocator), currentSegment(0) { }
	
	HelHandle space;
	uint8_t buffer[128];
	managarm::ld_server::ServerResponse<Allocator> response;
	size_t currentSegment;
};

auto loadObject =
frigg::asyncSeq(
	frigg::wrapFuncPtr<helx::RecvStringFunction>([](LoadContext *context,
			void *cb_object, auto cb_function,
			frigg::StringView object_name, uintptr_t base_address) {
		managarm::ld_server::ClientRequest<Allocator> request(*allocator);
		request.set_identifier(frigg::String<Allocator>(*allocator, object_name));
		request.set_base_address(base_address);
		
		frigg::String<Allocator> serialized(*allocator);
		request.SerializeToString(&serialized);
		ldServerPipe.sendString(serialized.data(), serialized.size(), 1, 0);
		
		ldServerPipe.recvString(context->buffer, 128, eventHub,
				1, 0, cb_object, cb_function);
	}),
	frigg::wrapFunctor([](LoadContext *context, auto &callback, HelError error,
			int64_t msg_request, int64_t msg_seq, size_t length) {
		HEL_CHECK(error);

		context->response.ParseFromArray(context->buffer, length);
		callback();
	}),
	frigg::asyncRepeatWhile(
		frigg::wrapFunctor([] (LoadContext *context, auto &callback) {
			callback(context->currentSegment < context->response.segments_size());
		}),
		frigg::asyncSeq(
			frigg::wrapFuncPtr<helx::RecvDescriptorFunction>([](LoadContext *context,
					void *cb_object, auto cb_function) {
				ldServerPipe.recvDescriptor(eventHub, 1, 1 + context->currentSegment,
						cb_object, cb_function);
			}),
			frigg::wrapFunctor([](LoadContext *context, auto &callback, HelError error,
					int64_t msg_request, int64_t msg_seq, HelHandle handle) {
				HEL_CHECK(error);

				auto &segment = context->response.segments(context->currentSegment);

				uint32_t map_flags = 0;
				if(segment.access() == managarm::ld_server::Access::READ_WRITE) {
					map_flags |= kHelMapReadWrite;
				}else{
					assert(segment.access() == managarm::ld_server::Access::READ_EXECUTE);
					map_flags |= kHelMapReadExecute | kHelMapShareOnFork;
				}
				
				void *actual_ptr;
				HEL_CHECK(helMapMemory(handle, context->space, (void *)segment.virt_address(),
						segment.virt_length(), map_flags, &actual_ptr));
				HEL_CHECK(helCloseDescriptor(handle));
				context->currentSegment++;
				callback();
			})
		)
	)
);

struct ExecuteContext {
	ExecuteContext(frigg::String<Allocator> program, StdSharedPtr<Process> process)
	: program(frigg::move(program)), process(process) {
		// reset the virtual memory space of the process
		if(process->vmSpace != kHelNullHandle)
			HEL_CHECK(helCloseDescriptor(process->vmSpace));
		HEL_CHECK(helCreateSpace(&process->vmSpace));
		executableContext.space = process->vmSpace;
		interpreterContext.space = process->vmSpace;
		process->iteration++;
	}

	frigg::String<Allocator> program;
	StdSharedPtr<Process> process;

	LoadContext executableContext;
	LoadContext interpreterContext;
};

auto executeProgram =
frigg::asyncSeq(
	frigg::wrapFunctor([](ExecuteContext *context, auto &callback) {
		callback(frigg::StringView(context->program.data(), context->program.size()), 0);
	}),
	frigg::subContext(&ExecuteContext::executableContext,
		frigg::wrapFunctor([](LoadContext *context, auto &callback,
				frigg::StringView object_name, uintptr_t base_address) {
			callback(object_name, base_address);
		})
	),
	frigg::subContext(&ExecuteContext::executableContext, loadObject),
	frigg::wrapFunctor([](ExecuteContext *context, auto &callback) {
		callback(frigg::StringView("ld-init.so"), 0x40000000);
	}),
	frigg::subContext(&ExecuteContext::interpreterContext, loadObject),
	frigg::wrapFunctor([](ExecuteContext *context, auto &callback) {
		constexpr size_t stack_size = 0x10000;
		
		HelHandle stack_memory;
		HEL_CHECK(helAllocateMemory(stack_size, kHelAllocOnDemand, &stack_memory));

		void *stack_base;
		HEL_CHECK(helMapMemory(stack_memory, context->process->vmSpace, nullptr,
				stack_size, kHelMapReadWrite, &stack_base));
		HEL_CHECK(helCloseDescriptor(stack_memory));

		HelThreadState state;
		memset(&state, 0, sizeof(HelThreadState));
		state.rip = context->interpreterContext.response.entry();
		state.rsp = (uintptr_t)stack_base + stack_size;
		state.rdi = context->executableContext.response.phdr_pointer();
		state.rsi = context->executableContext.response.phdr_entry_size();
		state.rdx = context->executableContext.response.phdr_count();
		state.rcx = context->executableContext.response.entry();

		helx::Directory directory = Process::runServer(context->process);

		HelHandle thread;
		HEL_CHECK(helCreateThread(context->process->vmSpace, directory.getHandle(),
				&state, kHelThreadNewUniverse, &thread));
		callback();
	})
);
	
void sendResponse(helx::Pipe &pipe, managarm::posix::ServerResponse<Allocator> &response,
		int64_t msg_request) {
	frigg::String<Allocator> serialized(*allocator);
	response.SerializeToString(&serialized);
	pipe.sendString(serialized.data(), serialized.size(), msg_request, 0);
}

// --------------------------------------------------------
// OpenClosure
// --------------------------------------------------------

struct OpenClosure : frigg::BaseClosure<OpenClosure> {
	OpenClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
			managarm::posix::ClientRequest<Allocator> request, int64_t msg_request);

	void operator() ();

private:
	void openComplete(StdSharedPtr<VfsOpenFile> file);

	StdSharedPtr<helx::Pipe> pipe;
	StdSharedPtr<Process> process;
	managarm::posix::ClientRequest<Allocator> request;
	int64_t msgRequest;
};

OpenClosure::OpenClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
		managarm::posix::ClientRequest<Allocator> request, int64_t msg_request)
: pipe(frigg::move(pipe)), process(frigg::move(process)), request(frigg::move(request)),
		msgRequest(msg_request) { }

void OpenClosure::operator() () {
	uint32_t open_flags = 0;
	if((request.flags() & managarm::posix::OpenFlags::CREAT) != 0)
		open_flags |= MountSpace::kOpenCreat;

	uint32_t open_mode = 0;
	if((request.mode() & managarm::posix::OpenMode::HELFD) != 0)
		open_mode |= MountSpace::kOpenHelfd;

	MountSpace *mount_space = process->mountSpace;
	mount_space->openAbsolute(process, request.path(), open_flags, open_mode,
			CALLBACK_MEMBER(this, &OpenClosure::openComplete));
}

void OpenClosure::openComplete(StdSharedPtr<VfsOpenFile> file) {
	assert(file);

	int fd = process->nextFd;
	assert(fd > 0);
	process->nextFd++;
	process->allOpenFiles.insert(fd, frigg::move(file));

	managarm::posix::ServerResponse<Allocator> response(*allocator);
	response.set_error(managarm::posix::Errors::SUCCESS);
	response.set_fd(fd);
	sendResponse(*pipe, response, msgRequest);
	suicide(*allocator);
}

// --------------------------------------------------------
// WriteClosure
// --------------------------------------------------------

struct WriteClosure : frigg::BaseClosure<WriteClosure> {
	WriteClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
			managarm::posix::ClientRequest<Allocator> request, int64_t msg_request);

	void operator() ();

private:
	void writeComplete();

	StdSharedPtr<helx::Pipe> pipe;
	StdSharedPtr<Process> process;
	managarm::posix::ClientRequest<Allocator> request;
	int64_t msgRequest;
};

WriteClosure::WriteClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
		managarm::posix::ClientRequest<Allocator> request, int64_t msg_request)
: pipe(frigg::move(pipe)), process(frigg::move(process)), request(frigg::move(request)),
		msgRequest(msg_request) { }

void WriteClosure::operator() () {
	auto file = process->allOpenFiles.get(request.fd());
	if(!file) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		sendResponse(*pipe, response, msgRequest);
		suicide(*allocator);
		return;
	}
	
	(*file)->write(request.buffer().data(), request.buffer().size(),
			CALLBACK_MEMBER(this, &WriteClosure::writeComplete));
}

void WriteClosure::writeComplete() {
	managarm::posix::ServerResponse<Allocator> response(*allocator);
	response.set_error(managarm::posix::Errors::SUCCESS);
	sendResponse(*pipe, response, msgRequest);
	suicide(*allocator);
}

// --------------------------------------------------------
// ReadClosure
// --------------------------------------------------------

struct ReadClosure : frigg::BaseClosure<ReadClosure> {
	ReadClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
			managarm::posix::ClientRequest<Allocator> request, int64_t msg_request);

	void operator() ();

private:
	void readComplete(size_t actual_size);

	StdSharedPtr<helx::Pipe> pipe;
	StdSharedPtr<Process> process;
	managarm::posix::ClientRequest<Allocator> request;
	int64_t msgRequest;
	
	frigg::String<Allocator> buffer;
};

ReadClosure::ReadClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process,
		managarm::posix::ClientRequest<Allocator> request, int64_t msg_request)
: pipe(frigg::move(pipe)), process(frigg::move(process)), request(frigg::move(request)),
		msgRequest(msg_request), buffer(*allocator) { }

void ReadClosure::operator() () {
	auto file = process->allOpenFiles.get(request.fd());
	if(!file) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		sendResponse(*pipe, response, msgRequest);
		suicide(*allocator);
		return;
	}
	
	buffer.resize(request.size());
	(*file)->read(buffer.data(), request.size(),
			CALLBACK_MEMBER(this, &ReadClosure::readComplete));
}

void ReadClosure::readComplete(size_t actual_size) {
	// TODO: make request.size() unsigned
	assert(actual_size == (size_t)request.size());
	managarm::posix::ServerResponse<Allocator> response(*allocator);
	response.set_buffer(frigg::move(buffer));
	response.set_error(managarm::posix::Errors::SUCCESS);
	sendResponse(*pipe, response, msgRequest);
	suicide(*allocator);
}

// --------------------------------------------------------
// RequestClosure
// --------------------------------------------------------

struct RequestClosure : frigg::BaseClosure<RequestClosure> {
	RequestClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process, int iteration)
	: pipe(frigg::move(pipe)), process(process), iteration(iteration) { }
	
	void operator() ();
	
private:
	void recvRequest(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	void processRequest(managarm::posix::ClientRequest<Allocator> request, int64_t msg_request);
	
	uint8_t buffer[128];
	StdSharedPtr<helx::Pipe> pipe;
	StdSharedPtr<Process> process;
	int iteration;
};

void RequestClosure::processRequest(managarm::posix::ClientRequest<Allocator> request,
		int64_t msg_request) {
	// check the iteration number to prevent this process from being hijacked
	if(process && iteration != process->iteration) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::DEAD_FORK);
		sendResponse(*pipe, response, msg_request);
		return;
	}

	if(request.request_type() == managarm::posix::ClientRequestType::INIT) {
		assert(!process);

		process = Process::init();

		auto device = frigg::makeShared<KernelOutDevice>(*allocator);

		unsigned int major, minor;
		DeviceAllocator &char_devices = process->mountSpace->charDevices;
		char_devices.allocateDevice("misc",
				frigg::staticPtrCast<Device>(frigg::move(device)), major, minor);

		auto fs = frigg::construct<dev_fs::MountPoint>(*allocator);
		auto inode = frigg::makeShared<dev_fs::CharDeviceNode>(*allocator, major, minor);
		fs->getRootDirectory()->entries.insert(frigg::String<Allocator>(*allocator, "helout"),
				frigg::staticPtrCast<dev_fs::Inode>(frigg::move(inode)));

		process->mountSpace->allMounts.insert(frigg::String<Allocator>(*allocator, "/dev"), fs);

		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(*pipe, response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::FORK) {
		StdSharedPtr<Process> new_process = process->fork();

		HelThreadState state;
		memset(&state, 0, sizeof(HelThreadState));
		state.rip = request.child_ip();
		state.rsp = request.child_sp();
		
		helx::Directory directory = Process::runServer(new_process);

		HelHandle thread;
		HEL_CHECK(helCreateThread(new_process->vmSpace, directory.getHandle(),
				&state, kHelThreadNewUniverse, &thread));

		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(*pipe, response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::EXEC) {
		frigg::runAsync<ExecuteContext>(*allocator, executeProgram,
				request.path(), process);
		
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(*pipe, response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::OPEN) {
		frigg::runClosure<OpenClosure>(*allocator, StdSharedPtr<helx::Pipe>(pipe),
				StdSharedPtr<Process>(process), frigg::move(request), msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::WRITE) {
		frigg::runClosure<WriteClosure>(*allocator, StdSharedPtr<helx::Pipe>(pipe),
				StdSharedPtr<Process>(process), frigg::move(request), msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::READ) {
		frigg::runClosure<ReadClosure>(*allocator, StdSharedPtr<helx::Pipe>(pipe),
				StdSharedPtr<Process>(process), frigg::move(request), msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::CLOSE) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);

		int32_t fd = request.fd();
		auto file_wrapper = process->allOpenFiles.get(fd);
		if(file_wrapper){
			process->allOpenFiles.remove(fd);
			response.set_error(managarm::posix::Errors::SUCCESS);
		}else{
			response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		}
		
		sendResponse(*pipe, response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::DUP2) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);

		int32_t oldfd = request.fd();
		int32_t newfd = request.newfd();
		auto file_wrapper = process->allOpenFiles.get(oldfd);
		if(file_wrapper){
			auto file = *file_wrapper;
			process->allOpenFiles.insert(newfd, file);

			response.set_error(managarm::posix::Errors::SUCCESS);
		}else{
			response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		}

		sendResponse(*pipe, response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::HELFD_ATTACH) {
		HelError error;
		HelHandle handle;
		//FIXME
		pipe->recvDescriptorSync(eventHub, msg_request, 1, error, handle);
		HEL_CHECK(error);

		auto file_wrapper = process->allOpenFiles.get(request.fd());
		if(!file_wrapper) {
			managarm::posix::ServerResponse<Allocator> response(*allocator);
			response.set_error(managarm::posix::Errors::NO_SUCH_FD);
			sendResponse(*pipe, response, msg_request);
			return;
		}

		auto file = *file_wrapper;
		file->setHelfd(handle);
		
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(*pipe, response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::HELFD_CLONE) {
		auto file_wrapper = process->allOpenFiles.get(request.fd());
		if(!file_wrapper) {
			managarm::posix::ServerResponse<Allocator> response(*allocator);
			response.set_error(managarm::posix::Errors::NO_SUCH_FD);
			sendResponse(*pipe, response, msg_request);
			return;
		}

		auto file = *file_wrapper;
		pipe->sendDescriptor(file->getHelfd(), msg_request, 1);
		
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(*pipe, response, msg_request);
	}else{
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);
		sendResponse(*pipe, response, msg_request);
	}
}

void RequestClosure::operator() () {
	auto callback = CALLBACK_MEMBER(this, &RequestClosure::recvRequest);
	pipe->recvString(buffer, 128, eventHub,
			kHelAnyRequest, 0, callback.getObject(), callback.getFunction());
}

void RequestClosure::recvRequest(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	if(error == kHelErrPipeClosed) {
		suicide(*allocator);
		return;
	}
	HEL_CHECK(error);

	managarm::posix::ClientRequest<Allocator> request(*allocator);
	request.ParseFromArray(buffer, length);
	processRequest(frigg::move(request), msg_request);

	(*this)();
}

void acceptLoop(helx::Server server, StdSharedPtr<Process> process, int iteration) {
	struct AcceptContext {
		AcceptContext(helx::Server server, StdSharedPtr<Process> process, int iteration)
		: server(frigg::move(server)), process(process), iteration(iteration) { }

		helx::Server server;
		StdSharedPtr<Process> process;
		int iteration;
	};

	auto body =
	frigg::asyncRepeatUntil(
		frigg::asyncSeq(
			frigg::wrapFuncPtr<helx::AcceptFunction>([] (AcceptContext *context,
					void *cb_object, auto cb_function) {
				context->server.accept(eventHub, cb_object, cb_function);
			}),
			frigg::wrapFunctor([] (AcceptContext *context, auto &callback,
					HelError error, HelHandle handle) {
				HEL_CHECK(error);
				
				auto pipe = frigg::makeShared<helx::Pipe>(*allocator, handle);
				frigg::runClosure<RequestClosure>(*allocator, frigg::move(pipe),
						context->process, context->iteration);

				callback(true);
			})
		)
	);

	frigg::runAsync<AcceptContext>(*allocator, body, frigg::move(server),
			process, iteration);
}

typedef void (*InitFuncPtr) ();
extern InitFuncPtr __init_array_start[];
extern InitFuncPtr __init_array_end[];

int main() {
	infoLogger.initialize(infoSink);
	infoLogger->log() << "Starting posix-subsystem" << frigg::EndLog();
	allocator.initialize(virtualAlloc);

	// we're using no libc, so we have to run constructors manually
	size_t init_count = __init_array_end - __init_array_start;
	for(size_t i = 0; i < init_count; i++)
		__init_array_start[i]();
	
	const char *ld_path = "local/rtdl-server";
	HelHandle ld_handle;
	HEL_CHECK(helRdOpen(ld_path, strlen(ld_path), &ld_handle));
	ldServerConnect = helx::Client(ld_handle);

	int64_t ld_connect_id;
	HEL_CHECK(helSubmitConnect(ldServerConnect.getHandle(), eventHub.getHandle(),
			0, 0, &ld_connect_id));
	HelError connect_error;
	eventHub.waitForConnect(ld_connect_id, connect_error, ldServerPipe);
	HEL_CHECK(connect_error);

	helx::Server server;
	helx::Client client;
	helx::Server::createServer(server, client);
	acceptLoop(frigg::move(server), StdSharedPtr<Process>(), 0);

	const char *parent_path = "local/parent";
	HelHandle parent_handle;
	HEL_CHECK(helRdOpen(parent_path, strlen(parent_path), &parent_handle));
	HEL_CHECK(helSendDescriptor(parent_handle, client.getHandle(), 0, 0));
	client.reset();

	while(true) {
		eventHub.defaultProcessEvents();
	}
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

