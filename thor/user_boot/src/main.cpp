
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

namespace util = frigg::util;
namespace debug = frigg::debug;
namespace async = frigg::async;

util::Tuple<uintptr_t, size_t> calcSegmentMap(uintptr_t address, size_t length) {
	size_t page_size = 0x1000;

	uintptr_t map_page = address / page_size;
	if(length == 0)
		return util::makeTuple(map_page * page_size, size_t(0));
	
	uintptr_t limit = address + length;
	uintptr_t num_pages = (limit / page_size) - map_page;
	if(limit % page_size != 0)
		num_pages++;
	
	return util::makeTuple(map_page * page_size, num_pages * page_size);
}

HelHandle loadSegment(void *image, uintptr_t address, uintptr_t file_offset,
		size_t mem_length, size_t file_length) {
	ASSERT(mem_length > 0);
	util::Tuple<uintptr_t, size_t> map = calcSegmentMap(address, mem_length);

	HelHandle memory;
	HEL_CHECK(helAllocateMemory(map.get<1>(), &memory));
	
	// map the segment memory as read/write and initialize it
	void *write_ptr;
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr, map.get<1>(),
			kHelMapReadWrite, &write_ptr));

	memset(write_ptr, 0, map.get<1>());
	memcpy((void *)((uintptr_t)write_ptr + (address - map.get<0>())),
			(void *)((uintptr_t)image + file_offset), file_length);
	
	// TODO: unmap the memory region

	return memory;
}

void mapSegment(HelHandle memory, HelHandle space, uintptr_t address,
		size_t length, uint32_t map_flags) {
	ASSERT(length > 0);
	util::Tuple<uintptr_t, size_t> map = calcSegmentMap(address, length);

	void *actual_ptr;
	HEL_CHECK(helMapMemory(memory, space, (void *)map.get<0>(), map.get<1>(),
			map_flags, &actual_ptr));
	ASSERT(actual_ptr == (void *)map.get<0>());
}

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
			uint32_t map_flags = 0;
			if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
				map_flags |= kHelMapReadWrite;
			}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
				map_flags |= kHelMapReadExecute;
			}else{
				debug::panicLogger.log() << "Illegal combination of segment permissions"
						<< debug::Finish();
			}

			HelHandle memory = loadSegment(image_ptr, phdr->p_vaddr,
					phdr->p_offset, phdr->p_memsz, phdr->p_filesz);
			mapSegment(memory, space, phdr->p_vaddr, phdr->p_memsz, map_flags);
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
helx::Directory initDirectory;

struct StartLdServerContext {
	StartLdServerContext()
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

auto startLdServer =
async::seq(
	async::lambda([](StartLdServerContext &context,
			util::Callback<void(HelError, int64_t, int64_t, HelHandle)> callback) {
		loadImage("initrd/ld-server", context.directory.getHandle());

		// receive a server handle from ld-server
		context.childPipe.recvDescriptor(eventHub, kHelAnyRequest, kHelAnySequence,
				callback.getObject(), callback.getFunction());
	}),
	async::lambda([](StartLdServerContext &context,
			util::Callback<void(HelError, HelHandle)> callback, HelError error,
			int64_t msg_request, int64_t msg_seq, HelHandle connect_handle) {
		HEL_CHECK(error);

		ldServerConnect = helx::Client(connect_handle);
		ldServerConnect.connect(eventHub, callback.getObject(), callback.getFunction());
	}),
	async::lambda([](StartLdServerContext &context, util::Callback<void()> callback,
			HelError error, HelHandle pipe_handle) {
		HEL_CHECK(error);

		ldServerPipe = helx::Pipe(pipe_handle);
		callback();
	})
);

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
	ExecuteContext(const char *program)
	: program(program), directory(helx::Directory::create()),
			localDirectory(helx::Directory::create()),
			configDirectory(helx::Directory::create()) {
		HEL_CHECK(helCreateSpace(&space));
		executableContext.space = space;
		interpreterContext.space = space;

		directory.mount(localDirectory.getHandle(), "local");
		directory.mount(configDirectory.getHandle(), "config");
		directory.mount(initDirectory.getHandle(), "init");
		directory.remount("initrd/#this", "initrd");
	}

	const char *program;
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

		callback(util::StringView(context.program), 0);
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

		infoLogger->log() << "Starting " << context.program << debug::Finish();
		HelHandle thread;
		HEL_CHECK(helCreateThread(context.space, context.directory.getHandle(),
				&state, &thread));
		callback();
	})
);

struct InstallServerContext {
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
);

struct InitContext {
	InitContext()
	: startAcpiContext("acpi", "hw"), startInitrdFsContext("initrd_fs", "initrd"),
			startZisaContext("zisa") { }

	StartLdServerContext startLdServerContext;
	InstallServerContext startAcpiContext;
	InstallServerContext startInitrdFsContext;
	ExecuteContext startZisaContext;
};

auto initialize =
async::seq(
	async::subContext(&InitContext::startLdServerContext, startLdServer),
	async::subContext(&InitContext::startAcpiContext, installServer),
	async::subContext(&InitContext::startInitrdFsContext, installServer),
	async::subContext(&InitContext::startZisaContext, executeProgram)
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

	initDirectory = helx::Directory::create();

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

