
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
#include <frigg/callback.hpp>
#include <frigg/async.hpp>
#include <frigg/protobuf.hpp>

#include <frigg/glue-hel.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#define assert ASSERT
#include "ld-server.frigg_pb.hpp"
#include "posix.frigg_pb.hpp"

namespace util = frigg::util;
namespace async = frigg::async;

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

auto loadObject = async::seq(
	async::lambda([](LoadContext &context,
			util::Callback<void(HelError, int64_t, int64_t, size_t)> callback,
			util::StringView object_name, uintptr_t base_address) {
		managarm::ld_server::ClientRequest<Allocator> request(*allocator);
		request.set_identifier(util::String<Allocator>(*allocator, object_name));
		request.set_base_address(base_address);
		
		util::String<Allocator> serialized(*allocator);
		request.SerializeToString(&serialized);
		ldServerPipe.sendString(serialized.data(), serialized.size(), 1, 0);
		
		ldServerPipe.recvString(context.buffer, 128, eventHub,
				1, 0, callback.getObject(), callback.getFunction());
	}),
	async::lambda([](LoadContext &context,
			util::Callback<void()> callback, HelError error,
			int64_t msg_request, int64_t msg_seq, size_t length) {
		HEL_CHECK(error);

		context.response.ParseFromArray(context.buffer, length);
		callback();
	}),
	async::repeatWhile(
		async::lambda([](LoadContext &context,
				util::Callback<void(bool)> callback) {
			callback(context.currentSegment < context.response.segments_size());
		}),
		async::seq(
			async::lambda([](LoadContext &context,
					util::Callback<void(HelError, int64_t, int64_t, HelHandle)> callback) {
				ldServerPipe.recvDescriptor(eventHub, 1, 1 + context.currentSegment,
						callback.getObject(), callback.getFunction());
			}),
			async::lambda([](LoadContext &context,
					util::Callback<void()> callback, HelError error,
					int64_t msg_request, int64_t msg_seq, HelHandle handle) {
				HEL_CHECK(error);

				auto &segment = context.response.segments(context.currentSegment);

				uint32_t map_flags = 0;
				if(segment.access() == managarm::ld_server::Access::READ_WRITE) {
					map_flags |= kHelMapReadWrite;
				}else{
					ASSERT(segment.access() == managarm::ld_server::Access::READ_EXECUTE);
					map_flags |= kHelMapReadExecute;
				}
				
				void *actual_ptr;
				HEL_CHECK(helMapMemory(handle, context.space, (void *)segment.virt_address(),
						segment.virt_length(), map_flags, &actual_ptr));
				context.currentSegment++;
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
async::seq(
	async::lambda([](ExecuteContext &context,
			util::Callback<void(util::StringView, uintptr_t)> callback) {
		context.configDirectory.publish(ldServerConnect.getHandle(), "rtdl-server");

		helx::Server server;
		helx::Client client;
		helx::Server::createServer(server, client);
		acceptProcess(server);

		context.localDirectory.publish(client.getHandle(), "posix");

		callback(util::StringView(context.program.data(), context.program.size()), 0);
	}),
	async::subContext(&ExecuteContext::executableContext,
		async::lambda([](LoadContext &context,
				util::Callback<void(util::StringView, uintptr_t)> callback,
				util::StringView object_name, uintptr_t base_address) {
			callback(object_name, base_address);
		})
	),
	async::subContext(&ExecuteContext::executableContext, loadObject),
	async::lambda([](ExecuteContext &context,
			util::Callback<void(util::StringView, uintptr_t)> callback) {
		callback(util::StringView("ld-init.so"), 0x40000000);
	}),
	async::subContext(&ExecuteContext::interpreterContext, loadObject),
	async::lambda([](ExecuteContext &context, util::Callback<void()> callback) {
		constexpr size_t stack_size = 0x200000;
		
		HelHandle stack_memory;
		HEL_CHECK(helAllocateMemory(stack_size, &stack_memory));

		void *stack_base;
		HEL_CHECK(helMapMemory(stack_memory, context.space, nullptr,
				stack_size, kHelMapReadWrite, &stack_base));

		HelThreadState state;
		memset(&state, 0, sizeof(HelThreadState));
		state.rip = context.interpreterContext.response.entry();
		state.rsp = (uintptr_t)stack_base + stack_size;
		state.rdi = context.executableContext.response.phdr_pointer();
		state.rsi = context.executableContext.response.phdr_entry_size();
		state.rdx = context.executableContext.response.phdr_count();
		state.rcx = context.executableContext.response.entry();

		HelHandle thread;
		HEL_CHECK(helCreateThread(context.space, context.directory.getHandle(),
				&state, &thread));
		callback();
	})
);

struct ProcessContext {
	ProcessContext(helx::Pipe pipe) : pipe(pipe) { }

	uint8_t buffer[128];
	helx::Pipe pipe;
};

auto processRequest = async::repeatWhile(
	async::lambda([] (ProcessContext &context, util::Callback<void(bool)> callback) {
		callback(true);
	}),
	async::seq(
		async::lambda([] (ProcessContext &context, util::Callback<void(HelError, int64_t,
				int64_t, size_t)> callback) {
			context.pipe.recvString(context.buffer, 128, eventHub,
					kHelAnyRequest, kHelAnySequence,
					callback.getObject(), callback.getFunction());
		}),
		async::lambda([] (ProcessContext &context, util::Callback<void()> callback, 
				HelError error, int64_t msg_request, int64_t msg_seq, size_t length) {
			HEL_CHECK(error);

			managarm::posix::ClientRequest<Allocator> request(*allocator);
			request.ParseFromArray(context.buffer, length);

			auto on_complete = [] (ExecuteContext &context) { };
			async::run(*allocator, executeProgram, ExecuteContext(request.path()), on_complete);

			callback();
		})
	)
);

void acceptProcess(helx::Server server) {
	struct AcceptContext {
		AcceptContext(helx::Server server) : server(server) { }

		helx::Server server;
	};

	auto acceptLoop = async::repeatWhile(
		async::lambda([] (AcceptContext &context, util::Callback<void(bool)> callback) {
			callback(true);
		}),
		async::seq(
			async::lambda([] (AcceptContext &context,
					util::Callback<void(HelError, HelHandle)> callback) {
				context.server.accept(eventHub, callback.getObject(), callback.getFunction());
			}),
			async::lambda([] (AcceptContext &context, util::Callback<void()> callback,
					HelError error, HelHandle handle) {
				HEL_CHECK(error);

				auto on_complete = [] (ProcessContext &context) { };
				helx::Pipe pipe(handle);
				async::run(*allocator, processRequest, ProcessContext(pipe), on_complete);

				callback();
			})
		)
	);

	auto on_complete = [] (AcceptContext &context) { };
	async::run(*allocator, acceptLoop, AcceptContext(server), on_complete);
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

