
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>
#include <frigg/libc.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/string.hpp>
#include <frigg/tuple.hpp>
#include <frigg/vector.hpp>
#include <frigg/callback.hpp>
#include <frigg/async.hpp>

#include <frigg/elf.hpp>
#include <frigg/protobuf.hpp>
#include <bragi-naked/ld-server.nakedpb.hpp>

#include <hel.h>
#include <hel-syscalls.h>

#include <frigg/glue-hel.hpp>

namespace util = frigg::util;
namespace debug = frigg::debug;
namespace async = frigg::async;
namespace protobuf = frigg::protobuf;

extern "C" void _exit() {
	helExitThisThread();
}

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
	helAllocateMemory(map.get<1>(), &memory);
	
	// map the segment memory as read/write and initialize it
	void *write_ptr;
	helMapMemory(memory, kHelNullHandle, nullptr, map.get<1>(),
			kHelMapReadWrite, &write_ptr);

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
	helMapMemory(memory, space, (void *)map.get<0>(), map.get<1>(),
			map_flags, &actual_ptr);
	ASSERT(actual_ptr == (void *)map.get<0>());
}

void loadImage(const char *path, HelHandle directory) {
	// open and map the executable image into this address space
	HelHandle image_handle;
	helRdOpen(path, strlen(path), &image_handle);

	size_t size;
	void *image_ptr;
	helMemoryInfo(image_handle, &size);
	helMapMemory(image_handle, kHelNullHandle, nullptr, size,
			kHelMapReadOnly, &image_ptr);
	
	// create a new 
	HelHandle space;
	helCreateSpace(&space);
	
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
		} //FIXME: handle other phdrs
	}
	
	constexpr size_t stack_size = 0x200000;
	
	HelHandle stack_memory;
	helAllocateMemory(stack_size, &stack_memory);

	void *stack_base;
	helMapMemory(stack_memory, space, nullptr,
			stack_size, kHelMapReadWrite, &stack_base);
	
	HelThreadState state;
	memset(&state, 0, sizeof(HelThreadState));
	state.rip = ehdr->e_entry;
	state.rsp = (uintptr_t)stack_base + stack_size;

	HelHandle thread;
	helCreateThread(space, directory, &state, &thread);
}

HelHandle eventHub;
HelHandle childHandle;

struct InitContext {
	InitContext(HelHandle directory)
	: directory(directory) { }

	HelHandle directory;
};

auto initialize =
async::seq(
	async::lambda([](InitContext &context,
			util::Callback<void(HelHandle)> callback) {
		// receive a server handle from ld-server
		int64_t async_id;
		helSubmitRecvDescriptor(childHandle, eventHub,
				kHelAnyRequest, kHelAnySequence,
				(uintptr_t)callback.getFunction(),
				(uintptr_t)callback.getObject(),
				&async_id);
	}),
	async::lambda([](InitContext &context,
			util::Callback<void(HelHandle)> callback, HelHandle connect_handle) {
		const char *name = "rtdl-server";
		helRdPublish(context.directory, name, strlen(name), connect_handle);

		// connect to the server
		int64_t async_id;
		helSubmitConnect(connect_handle, eventHub,
				(uintptr_t)callback.getFunction(),
				(uintptr_t)callback.getObject(),
				&async_id);
	})
);

struct LoadContext {
	struct Segment {
		uintptr_t virtAddress;
		size_t virtLength;
		int32_t access;
	};

	LoadContext(HelHandle pipe)
	: pipe(pipe), space(kHelNullHandle),
			segments(*allocator), currentSegment(0) { }
	
	LoadContext(const LoadContext &other) = delete;
	
	LoadContext(LoadContext &&other) = default;
	
	template<typename Reader>
	void parseObjectMsg(Reader reader) {
		while(!reader.atEnd()) {
			auto header = protobuf::fetchHeader(reader);
			switch(header.field) {
			case managarm::ld_server::ServerResponse::kField_phdr_pointer:
				phdrPointer = protobuf::fetchUInt64(reader);
				break;
			case managarm::ld_server::ServerResponse::kField_phdr_entry_size:
				phdrEntrySize = protobuf::fetchUInt64(reader);
				break;
			case managarm::ld_server::ServerResponse::kField_phdr_count:
				phdrCount = protobuf::fetchUInt64(reader);
				break;
			case managarm::ld_server::ServerResponse::kField_entry:
				entry = protobuf::fetchUInt64(reader);
				break;
			case managarm::ld_server::ServerResponse::kField_dynamic:
				// we don't care about the dynamic section
				protobuf::fetchUInt64(reader);
				break;
			case managarm::ld_server::ServerResponse::kField_segments:
				parseSegmentMsg(protobuf::fetchMessage(reader));
				break;
			default:
				ASSERT(!"Unexpected field in managarm.ld_server.Object message");
			}
		}
	}
	
	template<typename Reader>
	void parseSegmentMsg(Reader reader) {
		Segment segment;

		while(!reader.atEnd()) {
			auto header = protobuf::fetchHeader(reader);
			switch(header.field) {
			case managarm::ld_server::Segment::kField_virt_address:
				segment.virtAddress = protobuf::fetchUInt64(reader);
				break;
			case managarm::ld_server::Segment::kField_virt_length:
				segment.virtLength = protobuf::fetchUInt64(reader);
				break;
			case managarm::ld_server::Segment::kField_access:
				segment.access = protobuf::fetchInt32(reader);
				break;
			default:
				ASSERT(!"Unexpected field in managarm.ld_server.Segment message");
			}
		}

		segments.push(segment);
	}
	
	HelHandle pipe;
	HelHandle space;
	uintptr_t phdrPointer;
	size_t phdrEntrySize;
	size_t phdrCount;
	uintptr_t entry;
	util::Vector<Segment, Allocator> segments;
	size_t currentSegment;
	uint8_t buffer[128];
};

auto loadObject = async::seq(
	async::lambda([](LoadContext &context,
			util::Callback<void(size_t)> callback,
			util::StringView object_name, uintptr_t base_address) {
		protobuf::FixedWriter<64> writer;
		protobuf::emitString(writer,
				managarm::ld_server::ClientRequest::kField_identifier,
				object_name.data(), object_name.size());
		protobuf::emitUInt64(writer,
				managarm::ld_server::ClientRequest::kField_base_address,
				base_address);
		helSendString(context.pipe,
				writer.data(), writer.size(), 1, 0);

		int64_t async_id;
		helSubmitRecvString(context.pipe, eventHub,
				context.buffer, 128, 1, 0,
				(uintptr_t)callback.getFunction(),
				(uintptr_t)callback.getObject(),
				&async_id);
	}),
	async::lambda([](LoadContext &context,
			util::Callback<void()> callback, size_t length) {
		context.parseObjectMsg(protobuf::BufferReader(context.buffer, length));
		callback();
	}),
	async::repeatWhile(
		async::lambda([](LoadContext &context,
				util::Callback<void(bool)> callback) {
			callback(context.currentSegment < context.segments.size());
		}),
		async::seq(
			async::lambda([](LoadContext &context,
					util::Callback<void(HelHandle)> callback) {
				int64_t async_id;
				helSubmitRecvDescriptor(context.pipe, eventHub,
						1, 1 + context.currentSegment,
						(uintptr_t)callback.getFunction(),
						(uintptr_t)callback.getObject(),
						&async_id);
			}),
			async::lambda([](LoadContext &context,
					util::Callback<void()> callback, HelHandle handle) {
				auto &segment = context.segments[context.currentSegment];

				uint32_t map_flags = 0;
				if(segment.access == managarm::ld_server::Access::READ_WRITE) {
					map_flags |= kHelMapReadWrite;
				}else{
					ASSERT(segment.access == managarm::ld_server::Access::READ_EXECUTE);
					map_flags |= kHelMapReadExecute;
				}
				
				void *actual_ptr;
				helMapMemory(handle, context.space, (void *)segment.virtAddress,
						segment.virtLength, map_flags, &actual_ptr);

				infoLogger->log() << "Mapped segment" << debug::Finish();
				context.currentSegment++;
				callback();
			})
		)
	)
);

struct ExecuteContext {
	ExecuteContext(HelHandle pipe, HelHandle directory)
	: directory(directory), executableContext(pipe), interpreterContext(pipe) {
		helCreateSpace(&space);
		executableContext.space = space;
		interpreterContext.space = space;
	}

	ExecuteContext(const ExecuteContext &other) = delete;
	
	ExecuteContext(ExecuteContext &&other) = default;

	HelHandle space;
	HelHandle directory;
	LoadContext executableContext;
	LoadContext interpreterContext;
};

auto constructExecuteProgram() {
	return async::seq(
		async::lambda([](ExecuteContext &context,
				util::Callback<void(util::StringView, uintptr_t)> callback) {
			callback(util::StringView("acpi"), 0);
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
			helAllocateMemory(stack_size, &stack_memory);

			void *stack_base;
			helMapMemory(stack_memory, context.space, nullptr,
					stack_size, kHelMapReadWrite, &stack_base);

			HelThreadState state;
			memset(&state, 0, sizeof(HelThreadState));
			state.rip = context.interpreterContext.entry;
			state.rsp = (uintptr_t)stack_base + stack_size;
			state.rdi = context.executableContext.phdrPointer;
			state.rsi = context.executableContext.phdrEntrySize;
			state.rdx = context.executableContext.phdrCount;
			state.rcx = context.executableContext.entry;

			HelHandle thread;
			helCreateThread(context.space, context.directory, &state, &thread);
			callback();
		})
	);
}

void remount(HelHandle directory, const char *path, const char *target) {
	HelHandle handle;
	helRdOpen(path, strlen(path), &handle);
	helRdMount(directory, target, strlen(target), handle);
	infoLogger->log() << "Mounted " << path << " to " << target << debug::Finish();
}

void main() {
	infoLogger->log() << "Entering k_init" << debug::Finish();

	helCreateEventHub(&eventHub);
	
	HelHandle directory;
	helCreateRd(&directory);

	remount(directory, "initrd/#this", "initrd");

	const char *pipe_name = "k_init";
	HelHandle other_end;
	helCreateBiDirectionPipe(&childHandle, &other_end);
	helRdPublish(directory, pipe_name, strlen(pipe_name), other_end);

	loadImage("initrd/ld-server", directory);
	
	async::run(*allocator, initialize, InitContext(directory),
	[](InitContext &context, HelHandle pipe) {
		async::run(*allocator, constructExecuteProgram(), ExecuteContext(pipe, context.directory),
		[](ExecuteContext &context) {
			infoLogger->log() << "Execute successful" << debug::Finish();
		});
	});
	
	while(true) {
		HelEvent events[16];
		size_t num_items;
	
		helWaitForEvents(eventHub, events, 16, kHelWaitInfinite, &num_items);

		for(size_t i = 0; i < num_items; i++) {
			void *function = (void *)events[i].submitFunction;
			void *object = (void *)events[i].submitObject;
			
			switch(events[i].type) {
			case kHelEventRecvString: {
				typedef void (*FunctionPtr) (void *, size_t);
				((FunctionPtr)(function))(object, events[i].length);
			} break;
			case kHelEventRecvDescriptor: {
				typedef void (*FunctionPtr) (void *, HelHandle);
				((FunctionPtr)(function))(object, events[i].handle);
			} break;
			case kHelEventConnect: {
				typedef void (*FunctionPtr) (void *, HelHandle);
				((FunctionPtr)(function))(object, events[i].handle);
			} break;
			default:
				debug::panicLogger.log() << "Unexpected event type "
						<< events[i].type << debug::Finish();
			}
		}
	}
	
	helExitThisThread();
}

