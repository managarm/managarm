
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>
#include <frigg/libc.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/string.hpp>
#include <frigg/tuple.hpp>
#include <frigg/vector.hpp>
#include <frigg/callback.hpp>
#include <frigg/async.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include <frigg/glue-hel.hpp>
#include <frigg/elf.hpp>
#include <frigg/protobuf.hpp>

#define assert ASSERT
#include "ld-server.frigg_pb.hpp"
#include "posix.frigg_pb.hpp"

namespace util = frigg::util;
namespace debug = frigg::debug;
namespace async = frigg::async;

void loadImage(const char *path, HelHandle directory) {
	// open and map the executable image into this address space
	HelHandle image_handle;
	HEL_CHECK(helRdOpen(path, strlen(path), &image_handle));

	size_t size;
	void *image_ptr;
	HEL_CHECK(helMemoryInfo(image_handle, &size));
	HEL_CHECK(helMapMemory(image_handle, kHelNullHandle, nullptr, size,
			kHelMapReadOnly, &image_ptr));
	
	HelHandle space;
	HEL_CHECK(helCreateSpace(&space));
	
	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)image_ptr;
	ASSERT(ehdr->e_ident[0] == 0x7F
			&& ehdr->e_ident[1] == 'E'
			&& ehdr->e_ident[2] == 'L'
			&& ehdr->e_ident[3] == 'F');
	ASSERT(ehdr->e_type == ET_EXEC);
	
	for(int i = 0; i < ehdr->e_phnum; i++) {
		auto phdr = (Elf64_Phdr *)((uintptr_t)image_ptr + ehdr->e_phoff
				+ i * ehdr->e_phentsize);

		if(phdr->p_type == PT_LOAD) {
			const size_t kPageSize = 0x1000;

			// align virtual address and length to page size
			uintptr_t virt_address = phdr->p_vaddr;
			virt_address -= virt_address % kPageSize;

			size_t virt_length = (phdr->p_vaddr + phdr->p_memsz) - virt_address;
			if((virt_length % kPageSize) != 0)
				virt_length += kPageSize - virt_length % kPageSize;
			
			// map the segment memory as read/write and initialize it
			HelHandle memory;
			HEL_CHECK(helAllocateMemory(virt_length, &memory));
			
			void *write_ptr;
			HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr, virt_length,
					kHelMapReadWrite, &write_ptr));

			memset(write_ptr, 0, virt_length);
			memcpy((void *)((uintptr_t)write_ptr + (phdr->p_vaddr - virt_address)),
					(void *)((uintptr_t)image_ptr + phdr->p_offset), phdr->p_filesz);
			
			// map the segment memory to its own address space
			uint32_t map_flags = 0;
			if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
				map_flags |= kHelMapReadWrite;
			}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
				map_flags |= kHelMapReadExecute;
			}else{
				debug::panicLogger.log() << "Illegal combination of segment permissions"
						<< debug::Finish();
			}

			void *actual_ptr;
			HEL_CHECK(helMapMemory(memory, space, (void *)virt_address, virt_length,
					map_flags, &actual_ptr));
		}else if(phdr->p_type == PT_GNU_EH_FRAME
				|| phdr->p_type == PT_GNU_STACK) {
			// ignore the phdr
		}else{
			ASSERT(!"Unexpected PHDR");
		}
	}
	
	constexpr size_t stack_size = 0x200000;
	
	HelHandle stack_memory;
	HEL_CHECK(helAllocateMemory(stack_size, &stack_memory));

	void *stack_base;
	HEL_CHECK(helMapMemory(stack_memory, space, nullptr,
			stack_size, kHelMapReadWrite, &stack_base));
	
	HelThreadState state;
	memset(&state, 0, sizeof(HelThreadState));
	state.rip = ehdr->e_entry;
	state.rsp = (uintptr_t)stack_base + stack_size;

	HelHandle thread;
	HEL_CHECK(helCreateThread(space, directory, &state, &thread));
}

helx::EventHub eventHub;
helx::Client ldServerConnect;
helx::Pipe ldServerPipe;
helx::Pipe posixPipe;

struct StartFreeContext {
	StartFreeContext()
	: directory(helx::Directory::create()), localDirectory(helx::Directory::create()) {
		helx::Pipe parent_pipe;
		helx::Pipe::createBiDirection(childPipe, parent_pipe);
		localDirectory.publish(parent_pipe.getHandle(), "parent");

		directory.mount(localDirectory.getHandle(), "local");
		directory.remount("initrd/#this", "initrd");
	}

	helx::Directory directory;
	helx::Directory localDirectory;
	helx::Pipe childPipe;
};

auto startAcpi =
async::seq(
	async::lambda([](StartFreeContext &context,
			util::Callback<void(HelError, int64_t, int64_t, HelHandle)> callback) {
		loadImage("initrd/acpi", context.directory.getHandle());

		// receive a client handle from the posix subsystem
		context.childPipe.recvDescriptor(eventHub, kHelAnyRequest, kHelAnySequence,
				callback.getObject(), callback.getFunction());
	}),
	async::lambda([](StartFreeContext &context,
			util::Callback<void(HelError, HelHandle)> callback, HelError error,
			int64_t msg_request, int64_t msg_seq, HelHandle connect_handle) {
		HEL_CHECK(error);

//		helx::Client posix_connect(connect_handle);
//		posix_connect.connect(eventHub, callback.getObject(), callback.getFunction());
		callback(0, 0);
	}),
	async::lambda([](StartFreeContext &context, util::Callback<void()> callback,
			HelError error, HelHandle pipe_handle) {
		HEL_CHECK(error);

//		posixPipe = helx::Pipe(pipe_handle);
		callback();
	})
);

auto startLdServer =
async::seq(
	async::lambda([](StartFreeContext &context,
			util::Callback<void(HelError, int64_t, int64_t, HelHandle)> callback) {
		loadImage("initrd/ld-server", context.directory.getHandle());

		// receive a client handle from ld-server
		context.childPipe.recvDescriptor(eventHub, kHelAnyRequest, kHelAnySequence,
				callback.getObject(), callback.getFunction());
	}),
	async::lambda([](StartFreeContext &context,
			util::Callback<void(HelError, HelHandle)> callback, HelError error,
			int64_t msg_request, int64_t msg_seq, HelHandle connect_handle) {
		HEL_CHECK(error);

		ldServerConnect = helx::Client(connect_handle);
		ldServerConnect.connect(eventHub, callback.getObject(), callback.getFunction());
	}),
	async::lambda([](StartFreeContext &context, util::Callback<void()> callback,
			HelError error, HelHandle pipe_handle) {
		HEL_CHECK(error);

		ldServerPipe = helx::Pipe(pipe_handle);
		callback();
	})
);

auto startPosix =
async::seq(
	async::lambda([](StartFreeContext &context,
			util::Callback<void(HelError, int64_t, int64_t, HelHandle)> callback) {
		context.localDirectory.publish(ldServerConnect.getHandle(), "rtdl-server");

		loadImage("initrd/posix-subsystem", context.directory.getHandle());

		// receive a client handle from the posix subsystem
		context.childPipe.recvDescriptor(eventHub, kHelAnyRequest, kHelAnySequence,
				callback.getObject(), callback.getFunction());
	}),
	async::lambda([](StartFreeContext &context,
			util::Callback<void(HelError, HelHandle)> callback, HelError error,
			int64_t msg_request, int64_t msg_seq, HelHandle connect_handle) {
		HEL_CHECK(error);

		helx::Client posix_connect(connect_handle);
		posix_connect.connect(eventHub, callback.getObject(), callback.getFunction());
	}),
	async::lambda([](StartFreeContext &context, util::Callback<void()> callback,
			HelError error, HelHandle pipe_handle) {
		HEL_CHECK(error);

		posixPipe = helx::Pipe(pipe_handle);
		callback();
	})
);

struct PosixInitContext { };

auto runPosixInit =
async::seq(
	async::lambda([](PosixInitContext &context,
			util::Callback<void(HelError, int64_t, int64_t, size_t)> callback) {
		managarm::posix::ClientRequest<Allocator> request(*allocator);
		request.set_path(util::String<Allocator>(*allocator, "posix-init"));
		
		util::String<Allocator> serialized(*allocator);
		request.SerializeToString(&serialized);
		posixPipe.sendString(serialized.data(), serialized.size(), 1, 0);
		
//		posixPipe.recvString(context.buffer, 128, eventHub,
//				1, 0, callback.getObject(), callback.getFunction());
	}),
	async::lambda([](PosixInitContext &context,
			util::Callback<void()> callback, HelError error,
			int64_t msg_request, int64_t msg_seq, size_t length) {
		HEL_CHECK(error);

//		context.response.ParseFromArray(context.buffer, length);
//		callback();
	})
);

/*struct InstallServerContext {
	InstallServerContext(const char *program, const char *server_path)
	: executeContext(program), serverPath(server_path) {
		helx::Pipe parent_pipe;
		helx::Pipe::createBiDirection(childPipe, parent_pipe);
		executeContext.localDirectory.publish(parent_pipe.getHandle(), "parent");
	};

	ExecuteContext executeContext;
	const char *serverPath;
	helx::Pipe childPipe;
};

auto installServer =
async::seq(
	async::subContext(&InstallServerContext::executeContext,
			executeProgram),
	async::lambda([] (InstallServerContext &context,
			util::Callback<void(HelError, int64_t, int64_t, HelHandle)> callback) {
		context.childPipe.recvDescriptor(eventHub, kHelAnyRequest, kHelAnySequence,
				callback.getObject(), callback.getFunction());
	}),
	async::lambda([](InstallServerContext &context,
			util::Callback<void()> callback, HelError error,
			int64_t msg_request, int64_t msg_seq, HelHandle connect_handle) {
		HEL_CHECK(error);

		initDirectory.publish(connect_handle, context.serverPath);
		callback();
	})
);*/

struct InitContext {
	StartFreeContext startAcpiContext;
	StartFreeContext startLdServerContext;
	StartFreeContext startPosixContext;
	PosixInitContext posixInitContext;
};

auto initialize =
async::seq(
	async::subContext(&InitContext::startAcpiContext, startAcpi),
	async::subContext(&InitContext::startLdServerContext, startLdServer),
	async::subContext(&InitContext::startPosixContext, startPosix),
	async::subContext(&InitContext::posixInitContext, runPosixInit)
);

extern "C" void exit(int status) {
	HEL_CHECK(helExitThisThread());
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
	infoLogger->log() << "Entering user_boot" << debug::Finish();
	allocator.initialize(virtualAlloc);

	async::run(*allocator, initialize, InitContext(),
	[](InitContext &context) {
		infoLogger->log() << "user_boot finished successfully" << debug::Finish();
	});
	
	while(true)
		eventHub.defaultProcessEvents();
}

asm ( ".global _start\n"
		"_start:\n"
		"\tcall main\n"
		"\tud2" );

