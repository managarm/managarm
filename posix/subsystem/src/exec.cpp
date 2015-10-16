
#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/memory.hpp>
#include <frigg/debug.hpp>
#include <frigg/string.hpp>
#include <frigg/vector.hpp>
#include <frigg/async2.hpp>
#include <frigg/protobuf.hpp>
#include <frigg/glue-hel.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include "common.hpp"
#include "exec.hpp"

#include "ld-server.frigg_pb.hpp"

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
		ldServerPipe.sendStringReq(serialized.data(), serialized.size(), 1, 0);
		
		HEL_CHECK(ldServerPipe.recvStringResp(context->buffer, 128, eventHub,
				1, 0, cb_object, cb_function));
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
				ldServerPipe.recvDescriptorResp(eventHub, 1, 1 + context->currentSegment,
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
	ExecuteContext(frigg::StringView program, StdUnsafePtr<Process> process)
	: program(frigg::move(program)), process(process) {
		// reset the virtual memory space of the process
		if(process->vmSpace != kHelNullHandle)
			HEL_CHECK(helCloseDescriptor(process->vmSpace));
		HEL_CHECK(helCreateSpace(&process->vmSpace));
		executableContext.space = process->vmSpace;
		interpreterContext.space = process->vmSpace;
		process->iteration++;
	}

	frigg::StringView program;
	StdUnsafePtr<Process> process;

	LoadContext executableContext;
	LoadContext interpreterContext;
};

auto executeProgram =
frigg::asyncSeq(
	frigg::wrapFunctor([](ExecuteContext *context, auto &callback) {
		callback(context->program, 0);
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
		
		StdSharedPtr<Process> process(context->process);
		helx::Directory directory = Process::runServer(frigg::move(process));

		HelHandle thread;
		HEL_CHECK(helCreateThread(context->process->vmSpace, directory.getHandle(),
				&state, kHelThreadNewUniverse | kHelThreadNewGroup, &thread));
		HEL_CHECK(helCloseDescriptor(thread));
		callback();
	})
);

void execute(StdUnsafePtr<Process> process, frigg::StringView path) {
	frigg::runAsync<ExecuteContext>(*allocator, executeProgram, path, process);
}

