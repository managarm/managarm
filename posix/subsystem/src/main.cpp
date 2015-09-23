
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>
#include <frigg/libc.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
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

namespace util = frigg::util;

helx::EventHub eventHub;
helx::Client ldServerConnect;
helx::Pipe ldServerPipe;

struct OpenVfsNode {
	virtual OpenVfsNode *openRelative(util::StringView path);
	
	virtual void write(const void *buffer, size_t length);
	virtual void read(void *buffer, size_t max_length, size_t &actual_length);
};

OpenVfsNode *OpenVfsNode::openRelative(util::StringView path) {
	assert(!"Illegal operation for this file");
	__builtin_unreachable();
}
void OpenVfsNode::write(const void *buffer, size_t length) {
	assert(!"Illegal operation for this file");
}
void OpenVfsNode::read(void *buffer, size_t max_length, size_t &actual_length) {
	assert(!"Illegal operation for this file");
}

struct VfsMountPoint {
	virtual OpenVfsNode *open(util::StringView path) = 0;
};

struct MountSpace {
	MountSpace();

	OpenVfsNode *open(util::StringView path);

	util::Hashmap<util::String<Allocator>, VfsMountPoint *,
			util::DefaultHasher<util::StringView>, Allocator> allMounts;
};

MountSpace::MountSpace()
: allMounts(util::DefaultHasher<util::StringView>(), *allocator) { }

OpenVfsNode *MountSpace::open(util::StringView path) {
	assert(path.size() > 0);
	assert(path[0] == '/');
	
	// splits the path into a prefix that identifies the mount point
	// and a suffix that specifies remaining path relative to this mount point
	util::StringView prefix = path;
	util::StringView suffix;
	
	while(true) {
		auto mount = allMounts.get(prefix);
		if(mount)
			return (**mount)->open(suffix);

		if(prefix == "/")
			return nullptr;

		size_t seperator = prefix.findLast('/');
		assert(seperator != size_t(-1));
		prefix = path.subString(0, seperator);
		suffix = path.subString(seperator + 1, path.size() - (seperator + 1));
	}
};

struct Process {
	// creates a new process to run the "init" program
	static Process *init();

	Process();
	
	// creates a hel directory for this process
	helx::Directory buildConfiguration();

	// creates a new process by forking an old one
	Process *fork();
	
	// incremented when this process calls execve()
	// ensures that we don't accept new requests from old pipes after execve()
	int iteration;

	// mount namespace and virtual memory space of this process
	MountSpace *mountSpace;
	HelHandle vmSpace;

	util::Hashmap<int, OpenVfsNode *, util::DefaultHasher<int>, Allocator> allOpenFiles;
	int nextFd;
};

void acceptLoop(helx::Server server, Process *process, int iteration);

Process *Process::init() {
	auto new_process = frigg::memory::construct<Process>(*allocator);
	new_process->mountSpace = frigg::memory::construct<MountSpace>(*allocator);
	new_process->nextFd = 3; // reserve space for stdio

	return new_process;
}

Process::Process()
: iteration(0), mountSpace(nullptr), vmSpace(kHelNullHandle),
		allOpenFiles(util::DefaultHasher<int>(), *allocator), nextFd(-1) { }

helx::Directory Process::buildConfiguration() {
	auto directory = helx::Directory::create();
	auto localDirectory = helx::Directory::create();
	auto configDirectory = helx::Directory::create();
	
	directory.mount(configDirectory.getHandle(), "config");
	directory.mount(localDirectory.getHandle(), "local");

	configDirectory.publish(ldServerConnect.getHandle(), "rtdl-server");

	helx::Server server;
	helx::Client client;
	helx::Server::createServer(server, client);
	acceptLoop(server, this, iteration);
	localDirectory.publish(client.getHandle(), "posix");

	return directory;
}

Process *Process::fork() {
	auto new_process = frigg::memory::construct<Process>(*allocator);

	HEL_CHECK(helForkSpace(vmSpace, &new_process->vmSpace));

	for(auto it = allOpenFiles.iterator(); it; ++it)
		new_process->allOpenFiles.insert(it->get<0>(), it->get<1>());

	return new_process;
}

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
			util::StringView object_name, uintptr_t base_address) {
		managarm::ld_server::ClientRequest<Allocator> request(*allocator);
		request.set_identifier(util::String<Allocator>(*allocator, object_name));
		request.set_base_address(base_address);
		
		util::String<Allocator> serialized(*allocator);
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
					map_flags |= kHelMapReadExecute;
				}
				
				void *actual_ptr;
				HEL_CHECK(helMapMemory(handle, context->space, (void *)segment.virt_address(),
						segment.virt_length(), map_flags, &actual_ptr));
				context->currentSegment++;
				callback();
			})
		)
	)
);

struct ExecuteContext {
	ExecuteContext(util::String<Allocator> program, Process *process)
	: program(frigg::traits::move(program)), process(process) {
		// reset the virtual memory space of the process
		HEL_CHECK(helCreateSpace(&process->vmSpace));
		executableContext.space = process->vmSpace;
		interpreterContext.space = process->vmSpace;
		process->iteration++;
	}

	util::String<Allocator> program;
	Process *process;

	LoadContext executableContext;
	LoadContext interpreterContext;
};

auto executeProgram =
frigg::asyncSeq(
	frigg::wrapFunctor([](ExecuteContext *context, auto &callback) {
		callback(util::StringView(context->program.data(), context->program.size()), 0);
	}),
	frigg::subContext(&ExecuteContext::executableContext,
		frigg::wrapFunctor([](LoadContext *context, auto &callback,
				util::StringView object_name, uintptr_t base_address) {
			callback(object_name, base_address);
		})
	),
	frigg::subContext(&ExecuteContext::executableContext, loadObject),
	frigg::wrapFunctor([](ExecuteContext *context, auto &callback) {
		callback(util::StringView("ld-init.so"), 0x40000000);
	}),
	frigg::subContext(&ExecuteContext::interpreterContext, loadObject),
	frigg::wrapFunctor([](ExecuteContext *context, auto &callback) {
		constexpr size_t stack_size = 0x200000;
		
		HelHandle stack_memory;
		HEL_CHECK(helAllocateMemory(stack_size, &stack_memory));

		void *stack_base;
		HEL_CHECK(helMapMemory(stack_memory, context->process->vmSpace, nullptr,
				stack_size, kHelMapReadWrite, &stack_base));

		HelThreadState state;
		memset(&state, 0, sizeof(HelThreadState));
		state.rip = context->interpreterContext.response.entry();
		state.rsp = (uintptr_t)stack_base + stack_size;
		state.rdi = context->executableContext.response.phdr_pointer();
		state.rsi = context->executableContext.response.phdr_entry_size();
		state.rdx = context->executableContext.response.phdr_count();
		state.rcx = context->executableContext.response.entry();

		helx::Directory directory = context->process->buildConfiguration();

		HelHandle thread;
		HEL_CHECK(helCreateThread(context->process->vmSpace, directory.getHandle(),
				&state, kHelThreadNewUniverse, &thread));
		callback();
	})
);

struct KernelOutFile : public OpenVfsNode {
	virtual void write(const void *buffer, size_t length) override;
	virtual void read(void *buffer, size_t max_length, size_t &actual_length) override;
};

void KernelOutFile::write(const void *buffer, size_t length) {
	HEL_CHECK(helLog((const char *)buffer, length));
}

void KernelOutFile::read(void *buffer, size_t max_length,
		size_t &actual_length) {

}

namespace dev_fs {

struct MountPoint : public VfsMountPoint {
	virtual OpenVfsNode *open(util::StringView path) override;
};

struct DeviceNode : public OpenVfsNode {

};

OpenVfsNode *MountPoint::open(util::StringView path) {
	if(path == "helout") {
		return frigg::memory::construct<KernelOutFile>(*allocator);
	}else{
		return nullptr;
	}
}

}; // namespace dev_fs

struct RequestLoopContext {
	RequestLoopContext(helx::Pipe pipe, Process *process, int iteration)
	: pipe(pipe), process(process), iteration(iteration) { }
	
	void processRequest(managarm::posix::ClientRequest<Allocator> request, int64_t msg_request);
	
	void sendResponse(managarm::posix::ServerResponse<Allocator> &response, int64_t msg_request) {
		util::String<Allocator> serialized(*allocator);
		response.SerializeToString(&serialized);
		pipe.sendString(serialized.data(), serialized.size(), msg_request, 0);
	}

	uint8_t buffer[128];
	helx::Pipe pipe;
	Process *process;
	int iteration;
};

void RequestLoopContext::processRequest(managarm::posix::ClientRequest<Allocator> request,
		int64_t msg_request) {
	// check the iteration number to prevent this process from being hijacked
	if(process != nullptr && iteration != process->iteration) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::DEAD_FORK);
		sendResponse(response, msg_request);
		return;
	}

	if(request.request_type() == managarm::posix::ClientRequestType::INIT) {
		assert(process == nullptr);

		process = Process::init();
		process->mountSpace->allMounts.insert(util::String<Allocator>(*allocator,
				"/dev"), frigg::memory::construct<dev_fs::MountPoint>(*allocator));
		
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::FORK) {
		Process *new_process = process->fork();

		HelThreadState state;
		memset(&state, 0, sizeof(HelThreadState));
		state.rip = request.child_ip();
		state.rsp = request.child_sp();
		
		helx::Directory directory = new_process->buildConfiguration();

		HelHandle thread;
		HEL_CHECK(helCreateThread(new_process->vmSpace, directory.getHandle(),
				&state, kHelThreadNewUniverse, &thread));

		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::EXEC) {
		frigg::runAsync<ExecuteContext>(*allocator, executeProgram,
				request.path(), process);
		
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		sendResponse(response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::OPEN) {
		MountSpace *mount_space = process->mountSpace;
		OpenVfsNode *file = mount_space->open(request.path());
		assert(file != nullptr);

		int fd = process->nextFd;
		assert(fd > 0);
		process->nextFd++;
		process->allOpenFiles.insert(fd, file);

		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::SUCCESS);
		response.set_fd(fd);
		sendResponse(response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::READ) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);

		auto file_wrapper = process->allOpenFiles.get(request.fd());
		if(file_wrapper) {
			size_t actual_len;
			auto file = **file_wrapper;
			util::String<Allocator> buffer(*allocator);
			buffer.resize(request.size());
			file->read(buffer.data(), request.size(), actual_len);

			request.set_buffer(frigg::util::String<Allocator>(*allocator,
					buffer.data(), actual_len));
			response.set_error(managarm::posix::Errors::SUCCESS);
		}else{
			response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		}

		sendResponse(response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::WRITE) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);

		auto file_wrapper = process->allOpenFiles.get(request.fd());
		if(file_wrapper) {
			auto file = **file_wrapper;
			file->write(request.buffer().data(), request.buffer().size());

			response.set_error(managarm::posix::Errors::SUCCESS);
		}else{
			response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		}

		sendResponse(response, msg_request);
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
		
		sendResponse(response, msg_request);
	}else if(request.request_type() == managarm::posix::ClientRequestType::DUP2) {
		managarm::posix::ServerResponse<Allocator> response(*allocator);

		int32_t oldfd = request.fd();
		int32_t newfd = request.newfd();
		auto file_wrapper = process->allOpenFiles.get(oldfd);
		if(file_wrapper){
			auto file = **file_wrapper;
			process->allOpenFiles.insert(newfd, file);

			response.set_error(managarm::posix::Errors::SUCCESS);
		}else{
			response.set_error(managarm::posix::Errors::NO_SUCH_FD);
		}

		sendResponse(response, msg_request);
	}else{
		managarm::posix::ServerResponse<Allocator> response(*allocator);
		response.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);
		sendResponse(response, msg_request);
	}
}

auto requestLoop =
frigg::asyncRepeatUntil(
	frigg::asyncSeq(
		frigg::wrapFuncPtr<helx::RecvStringFunction>([] (RequestLoopContext *context,
				void *cb_object, auto cb_function) {
			context->pipe.recvString(context->buffer, 128, eventHub,
					kHelAnyRequest, 0, cb_object, cb_function);
		}),
		frigg::wrapFunctor([] (RequestLoopContext *context, auto &callback,
				HelError error, int64_t msg_request, int64_t msg_seq, size_t length) {
			HEL_CHECK(error);

			managarm::posix::ClientRequest<Allocator> request(*allocator);
			request.ParseFromArray(context->buffer, length);
			context->processRequest(frigg::traits::move(request), msg_request);

			callback(true);
		})
	)
);

void acceptLoop(helx::Server server, Process *process, int iteration) {
	struct AcceptContext {
		AcceptContext(helx::Server server, Process *process, int iteration)
		: server(server), process(process), iteration(iteration) { }

		helx::Server server;
		Process *process;
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
				
				frigg::runAsync<RequestLoopContext>(*allocator, requestLoop,
						helx::Pipe(handle), context->process, context->iteration);

				callback(true);
			})
		)
	);

	frigg::runAsync<AcceptContext>(*allocator, body, server, process, iteration);
}

typedef void (*InitFuncPtr) ();
extern InitFuncPtr __init_array_start[];
extern InitFuncPtr __init_array_end[];

int main() {
	infoLogger.initialize(infoSink);
	infoLogger->log() << "Starting posix-subsystem" << frigg::debug::Finish();
	allocator.initialize(virtualAlloc);

	// we're using no libc, so we have to run constructors manually
	size_t init_count = __init_array_end - __init_array_start;
	for(size_t i = 0; i < init_count; i++)
		__init_array_start[i]();
	
	const char *ld_path = "local/rtdl-server";
	HelHandle ld_handle;
	HEL_CHECK(helRdOpen(ld_path, strlen(ld_path), &ld_handle));
	ldServerConnect = ld_handle;

	int64_t ld_connect_id;
	HEL_CHECK(helSubmitConnect(ldServerConnect.getHandle(), eventHub.getHandle(),
			0, 0, &ld_connect_id));
	ldServerPipe = eventHub.waitForConnect(ld_connect_id);

	helx::Server server;
	helx::Client client;
	helx::Server::createServer(server, client);
	acceptLoop(server, nullptr, 0);

	const char *parent_path = "local/parent";
	HelHandle parent_handle;
	HEL_CHECK(helRdOpen(parent_path, strlen(parent_path), &parent_handle));
	HEL_CHECK(helSendDescriptor(parent_handle, client.getHandle(), 0, 0));

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

