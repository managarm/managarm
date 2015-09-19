
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

void acceptProcess(helx::Server server);

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
	ExecuteContext(util::String<Allocator> program)
	: program(frigg::traits::move(program)), directory(helx::Directory::create()),
			localDirectory(helx::Directory::create()),
			configDirectory(helx::Directory::create()) {
		HEL_CHECK(helCreateSpace(&space));
		executableContext.space = space;
		interpreterContext.space = space;

		directory.mount(localDirectory.getHandle(), "local");
		directory.mount(configDirectory.getHandle(), "config");
		directory.remount("initrd/#this", "initrd");
	}

	util::String<Allocator> program;
	HelHandle space;

	helx::Directory directory;
	helx::Directory localDirectory;
	helx::Directory configDirectory;
	LoadContext executableContext;
	LoadContext interpreterContext;
};

auto executeProgram =
frigg::asyncSeq(
	frigg::wrapFunctor([](ExecuteContext *context, auto &callback) {
		context->configDirectory.publish(ldServerConnect.getHandle(), "rtdl-server");

		helx::Server server;
		helx::Client client;
		helx::Server::createServer(server, client);
		acceptProcess(server);

		context->localDirectory.publish(client.getHandle(), "posix");

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
		HEL_CHECK(helMapMemory(stack_memory, context->space, nullptr,
				stack_size, kHelMapReadWrite, &stack_base));

		HelThreadState state;
		memset(&state, 0, sizeof(HelThreadState));
		state.rip = context->interpreterContext.response.entry();
		state.rsp = (uintptr_t)stack_base + stack_size;
		state.rdi = context->executableContext.response.phdr_pointer();
		state.rsi = context->executableContext.response.phdr_entry_size();
		state.rdx = context->executableContext.response.phdr_count();
		state.rcx = context->executableContext.response.entry();

		HelHandle thread;
		HEL_CHECK(helCreateThread(context->space, context->directory.getHandle(),
				&state, &thread));
		callback();
	})
);

struct OpenFile {
	virtual void write(const void *buffer, size_t length) = 0;
};

util::Hashmap<int, OpenFile *,
		util::DefaultHasher<int>, Allocator> 
		allOpenFiles(util::DefaultHasher<int>(), *allocator);

int nextFd = 3;

struct KernelOutFile : public OpenFile {
	virtual void write(const void *buffer, size_t length) override;
};

void KernelOutFile::write(const void *buffer, size_t length) {
	HEL_CHECK(helLog((const char *)buffer, length));
}

struct ProcessContext {
	ProcessContext(helx::Pipe pipe) : pipe(pipe) { }

	uint8_t buffer[128];
	helx::Pipe pipe;
};

auto processRequest =
frigg::asyncRepeatUntil(
	frigg::asyncSeq(
		frigg::wrapFuncPtr<helx::RecvStringFunction>([] (ProcessContext *context,
				void *cb_object, auto cb_function) {
			context->pipe.recvString(context->buffer, 128, eventHub,
					kHelAnyRequest, kHelAnySequence,
					cb_object, cb_function);
		}),
		frigg::wrapFunctor([] (ProcessContext *context, auto &callback,
				HelError error, int64_t msg_request, int64_t msg_seq, size_t length) {
			HEL_CHECK(error);

			managarm::posix::ClientRequest<Allocator> request(*allocator);
			request.ParseFromArray(context->buffer, length);

			if(request.request_type() == managarm::posix::ClientRequestType::SPAWN) {
				frigg::runAsync<ExecuteContext>(*allocator, executeProgram, request.path());
				
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::SUCCESS);

				util::String<Allocator> serialized(*allocator);
				response.SerializeToString(&serialized);
				context->pipe.sendString(serialized.data(), serialized.size(),
						msg_request, 0);
			}else if(request.request_type() == managarm::posix::ClientRequestType::OPEN) {
				int fd = nextFd;
				nextFd++;

				auto file = frigg::memory::construct<KernelOutFile>(*allocator);
				allOpenFiles.insert(fd, file);

				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::SUCCESS);
				response.set_fd(fd);

				util::String<Allocator> serialized(*allocator);
				response.SerializeToString(&serialized);
				context->pipe.sendString(serialized.data(), serialized.size(),
						msg_request, 0);
			}else if(request.request_type() == managarm::posix::ClientRequestType::WRITE) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				auto file_wrapper = allOpenFiles.get(request.fd());
				if(file_wrapper) {
					auto file = **file_wrapper;
					file->write(request.buffer().data(), request.buffer().size());

					response.set_error(managarm::posix::Errors::SUCCESS);
				}else{
					response.set_error(managarm::posix::Errors::NO_SUCH_FD);
				}

				util::String<Allocator> serialized(*allocator);
				response.SerializeToString(&serialized);
				context->pipe.sendString(serialized.data(), serialized.size(),
						msg_request, 0);
			}else{
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);

				util::String<Allocator> serialized(*allocator);
				response.SerializeToString(&serialized);
				context->pipe.sendString(serialized.data(), serialized.size(),
						msg_request, 0);
			}

			callback(true);
		})
	)
);

void acceptProcess(helx::Server server) {
	struct AcceptContext {
		AcceptContext(helx::Server server) : server(server) { }

		helx::Server server;
	};

	auto acceptLoop =
	frigg::asyncRepeatUntil(
		frigg::asyncSeq(
			frigg::wrapFuncPtr<helx::AcceptFunction>([] (AcceptContext *context,
					void *cb_object, auto cb_function) {
				context->server.accept(eventHub, cb_object, cb_function);
			}),
			frigg::wrapFunctor([] (AcceptContext *context, auto &callback,
					HelError error, HelHandle handle) {
				HEL_CHECK(error);
				
				frigg::runAsync<ProcessContext>(*allocator, processRequest,
						helx::Pipe(handle));

				callback(true);
			})
		)
	);

	frigg::runAsync<AcceptContext>(*allocator, acceptLoop, server);
}

typedef void (*InitFuncPtr) ();
extern InitFuncPtr __init_array_start[];
extern InitFuncPtr __init_array_end[];

int main() {
	// we're using no libc, so we have to run constructors manually
	size_t init_count = __init_array_end - __init_array_start;
	for(size_t i = 0; i < init_count; i++)
		__init_array_start[i]();
	
	infoLogger.initialize(infoSink);
	infoLogger->log() << "Starting posix-subsystem" << frigg::debug::Finish();
	allocator.initialize(virtualAlloc);
	
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
	acceptProcess(server);

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

}

void *__dso_handle;

