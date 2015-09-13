
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>
#include <frigg/support.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/libc.hpp>
#include <frigg/optional.hpp>
#include <frigg/string.hpp>
#include <frigg/variant.hpp>
#include <frigg/vector.hpp>
#include <frigg/hashmap.hpp>
#include <frigg/callback.hpp>
#include <frigg/async.hpp>
#include <frigg/elf.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include <frigg/glue-hel.hpp>
#include <frigg/protobuf.hpp>

#define assert ASSERT
#include "ld-server.frigg_pb.hpp"

namespace debug = frigg::debug;
namespace util = frigg::util;
namespace memory = frigg::memory;
namespace async = frigg::async;

struct BaseSegment {
	BaseSegment(Elf64_Word elf_type, Elf64_Word elf_flags,
			uintptr_t virt_address, size_t virt_length)
	: elfType(elf_type), elfFlags(elf_flags),
			virtAddress(virt_address), virtLength(virt_length) { }

	Elf64_Word elfType;
	Elf64_Word elfFlags;
	uintptr_t virtAddress;
	size_t virtLength;
};

struct SharedSegment : BaseSegment {
	SharedSegment(Elf64_Word elf_type, Elf64_Word elf_flags,
			uintptr_t virt_address, size_t virt_length, HelHandle memory)
	: BaseSegment(elf_type, elf_flags, virt_address, virt_length),
			memory(memory) { }
	
	HelHandle memory;
};

struct UniqueSegment : BaseSegment {
	UniqueSegment(Elf64_Word elf_type, Elf64_Word elf_flags,
			uintptr_t virt_address, size_t virt_length,
			uintptr_t file_disp, uintptr_t file_offset, size_t file_length)
	: BaseSegment(elf_type, elf_flags, virt_address, virt_length),
			fileDisplacement(file_disp), fileOffset(file_offset),
			fileLength(file_length) { }
	
	uintptr_t fileDisplacement;
	uintptr_t fileOffset;
	size_t fileLength;
};

typedef util::Variant<SharedSegment,
		UniqueSegment> Segment;

struct Object {
	Object()
	: phdrPointer(0), phdrEntrySize(0), phdrCount(0),
		entry(0), dynamic(0), segments(*allocator),
		hasPhdrImage(false) { }

	void *imagePtr;
	uintptr_t phdrPointer, phdrEntrySize, phdrCount;
	uintptr_t entry;
	uintptr_t dynamic;
	util::Vector<Segment, Allocator> segments;
	bool hasPhdrImage;
};

typedef util::Hashmap<const char *, Object *,
		util::CStringHasher, Allocator> objectMap;

Object *readObject(util::StringView path) {
	util::String<Allocator> full_path(*allocator, "initrd/");
	full_path += path;

	// open and map the executable image into this address space
	HelHandle image_handle;
	HEL_CHECK(helRdOpen(full_path.data(), full_path.size(), &image_handle));

	size_t image_size;
	void *image_ptr;
	HEL_CHECK(helMemoryInfo(image_handle, &image_size));
	HEL_CHECK(helMapMemory(image_handle, kHelNullHandle, nullptr, image_size,
			kHelMapReadOnly, &image_ptr));
	
	constexpr size_t kPageSize = 0x1000;
	
	// parse the ELf file format
	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)image_ptr;
	ASSERT(ehdr->e_ident[0] == 0x7F
			&& ehdr->e_ident[1] == 'E'
			&& ehdr->e_ident[2] == 'L'
			&& ehdr->e_ident[3] == 'F');
	ASSERT(ehdr->e_type == ET_EXEC || ehdr->e_type == ET_DYN);

	Object *object = memory::construct<Object>(*allocator);
	object->imagePtr = image_ptr;
	object->entry = ehdr->e_entry;

	for(int i = 0; i < ehdr->e_phnum; i++) {
		auto phdr = (Elf64_Phdr *)((uintptr_t)image_ptr + ehdr->e_phoff
				+ i * ehdr->e_phentsize);
		
		if(phdr->p_type == PT_PHDR) {
			object->phdrPointer = phdr->p_vaddr;
			object->hasPhdrImage = true;
			
			ASSERT(phdr->p_memsz == ehdr->e_phnum * (size_t)ehdr->e_phentsize);
			object->phdrEntrySize = ehdr->e_phentsize;
			object->phdrCount = ehdr->e_phnum;
		}else if(phdr->p_type == PT_LOAD) {
			bool can_share = false;
			if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
				// we cannot share this segment
			}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
				can_share = true;
			}else{
				debug::panicLogger.log() << "Illegal combination of segment permissions"
						<< debug::Finish();
			}

			ASSERT(phdr->p_memsz > 0);
			
			// align virtual address and length to page size
			uintptr_t virt_address = phdr->p_vaddr;
			virt_address -= virt_address % kPageSize;

			size_t virt_length = (phdr->p_vaddr + phdr->p_memsz) - virt_address;
			if((virt_length % kPageSize) != 0)
				virt_length += kPageSize - virt_length % kPageSize;
			
			uintptr_t displacement = phdr->p_vaddr - virt_address;
			if(can_share) {
				HelHandle memory;
				HEL_CHECK(helAllocateMemory(virt_length, &memory));

				void *map_pointer;
				HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
						virt_length, kHelMapReadWrite, &map_pointer));

				memset(map_pointer, 0, virt_length);
				memcpy((void *)((uintptr_t)map_pointer + displacement),
						(void *)((uintptr_t)image_ptr + phdr->p_offset),
						phdr->p_filesz);
				
				object->segments.push(SharedSegment(phdr->p_type,
						phdr->p_flags, virt_address, virt_length, memory));
			}else{
				object->segments.push(UniqueSegment(phdr->p_type,
						phdr->p_flags, virt_address, virt_length,
						displacement, phdr->p_offset, phdr->p_filesz));
			}
		}else if(phdr->p_type == PT_TLS) {
			// TODO: handle thread-local data
		}else if(phdr->p_type == PT_DYNAMIC) {
			object->dynamic = phdr->p_vaddr;
		}else if(phdr->p_type == PT_INTERP) {
			// TODO: handle different interpreters
		}else if(phdr->p_type == PT_GNU_EH_FRAME
				|| phdr->p_type == PT_GNU_STACK) {
			// ignore the phdr
		}else{
			ASSERT(!"Unexpected PHDR");
		}
	}
	
	return object;
}

void sendObject(HelHandle pipe, int64_t request_id,
		Object *object, uintptr_t base_address) {
	managarm::ld_server::ServerResponse<Allocator> response(*allocator);

	if(object->hasPhdrImage) {
		response.set_phdr_pointer(base_address + object->phdrPointer);
		response.set_phdr_entry_size(object->phdrEntrySize);
		response.set_phdr_count(object->phdrCount);
	}
	response.set_entry(base_address + object->entry);
	response.set_dynamic(base_address + object->dynamic);

	for(size_t i = 0; i < object->segments.size(); i++) {
		Segment &wrapper = object->segments[i];
		
		BaseSegment *base_segment;
		HelHandle memory;
		if(wrapper.is<SharedSegment>()) {
			auto &segment = wrapper.get<SharedSegment>();
			base_segment = &segment;
			
			memory = segment.memory;
		}else{
			ASSERT(wrapper.is<UniqueSegment>());
			auto &segment = wrapper.get<UniqueSegment>();
			base_segment = &segment;

			HEL_CHECK(helAllocateMemory(segment.virtLength, &memory));

			void *map_pointer;
			HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
					segment.virtLength, kHelMapReadWrite, &map_pointer));
			memset(map_pointer, 0, segment.virtLength);
			memcpy((void *)((uintptr_t)map_pointer + segment.fileDisplacement),
					(void *)((uintptr_t)object->imagePtr + segment.fileOffset),
					segment.fileLength);
		}
		
		managarm::ld_server::Segment<Allocator> out_segment(*allocator);
		out_segment.set_virt_address(base_address + base_segment->virtAddress);
		out_segment.set_virt_length(base_segment->virtLength);

		if((base_segment->elfFlags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
			out_segment.set_access(managarm::ld_server::Access::READ_WRITE);
		}else if((base_segment->elfFlags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
			out_segment.set_access(managarm::ld_server::Access::READ_EXECUTE);
		}else{
			debug::panicLogger.log() << "Illegal combination of segment permissions"
					<< debug::Finish();
		}
		
		response.add_segments(out_segment);
		
		HEL_CHECK(helSendDescriptor(pipe, memory, 1, 1 + i));
	}

	util::String<Allocator> serialized(*allocator);
	response.SerializeToString(&serialized);

	HEL_CHECK(helSendString(pipe, (uint8_t *)serialized.data(), serialized.size(), 1, 0));
}

util::LazyInitializer<helx::EventHub> eventHub;
util::LazyInitializer<helx::Server> server;

struct ProcessContext {
	ProcessContext(HelHandle pipe_handle)
	: pipeHandle(pipe_handle) { }

	HelHandle pipeHandle;
	uint8_t buffer[128];
};

auto processRequests =
async::repeatWhile(
	async::lambda([](ProcessContext &context, util::Callback<void(bool)> callback) {
		callback(true);
	}),
	async::seq(
		async::lambda([](ProcessContext &context,
				util::Callback<void(HelError, int64_t, int64_t, size_t)> callback) {
			int64_t async_id;
			HEL_CHECK(helSubmitRecvString(context.pipeHandle, eventHub->getHandle(),
					context.buffer, 128, kHelAnyRequest, 0,
					(uintptr_t)callback.getFunction(),
					(uintptr_t)callback.getObject(),
					&async_id));
		}),
		async::lambda([](ProcessContext &context, util::Callback<void()> callback,
				HelError error, int64_t msg_request, int64_t msg_sequence, size_t length) {
			HEL_CHECK(error);

			managarm::ld_server::ClientRequest<Allocator> request(*allocator);
			request.ParseFromArray(context.buffer, length);
			
			Object *object = readObject(util::StringView(request.identifier().data(),
					request.identifier().size()));
			sendObject(context.pipeHandle, msg_request, object, request.base_address());
			callback();
		})
	)
);

void onAccept(void *object, HelError error, HelHandle pipe_handle) {
	async::run(*allocator, processRequests, ProcessContext(pipe_handle),
		[](ProcessContext &context) { });
	
	server->accept(*eventHub, nullptr, &onAccept);
}

extern "C" void _exit(int status) {
	HEL_CHECK(helExitThisThread());
}

int main() {
	infoLogger.initialize(infoSink);
	infoLogger->log() << "Entering ld-server" << debug::Finish();
	allocator.initialize(virtualAlloc);

	eventHub.initialize();

	// create a server and listen for requests
	HelHandle serve_handle, client_handle;
	HEL_CHECK(helCreateServer(&serve_handle, &client_handle));

	server.initialize(serve_handle);
	server->accept(*eventHub, nullptr, &onAccept);
	
	// inform user_boot that we are ready to server requests
	const char *path = "local/parent";
	HelHandle parent_handle;
	HEL_CHECK(helRdOpen(path, strlen(path), &parent_handle));

	helx::Pipe parent_pipe(parent_handle);
	parent_pipe.sendDescriptor(client_handle, 1, 0);

	infoLogger->log() << "ld-server initialized succesfully!" << debug::Finish();

	while(true)
		eventHub->defaultProcessEvents();
	
	return 0;
}

