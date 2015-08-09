
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/initializer.hpp>
#include <frigg/support.hpp>
#include <frigg/debug.hpp>
#include <frigg/memory.hpp>
#include <frigg/libc.hpp>
#include <frigg/variant.hpp>
#include <frigg/vector.hpp>
#include <frigg/hashmap.hpp>
#include <frigg/elf.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include <frigg/glue-hel.hpp>

namespace debug = frigg::debug;
namespace util = frigg::util;
namespace memory = frigg::memory;

struct BaseSegment {
	BaseSegment(Elf64_Word elf_type, Elf64_Word elf_flags,
			uintptr_t virt_offset, size_t virt_length)
	: elfType(elf_type), elfFlags(elf_flags),
			virtOffset(virt_offset), virtLength(virt_length) { }

	Elf64_Word elfType;
	Elf64_Word elfFlags;
	uintptr_t virtOffset;
	size_t virtLength;
};

struct SharedSegment : BaseSegment {
	SharedSegment(Elf64_Word elf_type, Elf64_Word elf_flags,
			uintptr_t virt_offset, size_t virt_length, HelHandle memory)
	: BaseSegment(elf_type, elf_flags, virt_offset, virt_length),
			memory(memory) { }
	
	HelHandle memory;
};

struct UniqueSegment : BaseSegment {
	UniqueSegment(Elf64_Word elf_type, Elf64_Word elf_flags,
			uintptr_t virt_offset, size_t virt_length,
			uintptr_t file_disp, uintptr_t file_offset, size_t file_length)
	: BaseSegment(elf_type, elf_flags, virt_offset, virt_length),
			fileDisplacement(file_disp), fileOffset(file_offset),
			fileLength(file_length) { }
	
	uintptr_t fileDisplacement;
	uintptr_t fileOffset;
	size_t fileLength;
};

typedef util::Variant<SharedSegment,
		UniqueSegment> Segment;

struct Object {
	Object() : segments(*allocator) { }

	void *imagePtr;
	util::Vector<Segment, Allocator> segments;
};

typedef util::Hashmap<const char *, Object *,
		util::CStringHasher, Allocator> objectMap;

Object *readObject(const char *path) {
	// open and map the executable image into this address space
	HelHandle image_handle;
	helRdOpen(path, strlen(path), &image_handle);

	size_t image_size;
	void *image_ptr;
	helMemoryInfo(image_handle, &image_size);
	helMapMemory(image_handle, kHelNullHandle, nullptr, image_size,
			kHelMapReadOnly, &image_ptr);
	
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

	for(int i = 0; i < ehdr->e_phnum; i++) {
		auto phdr = (Elf64_Phdr *)((uintptr_t)image_ptr + ehdr->e_phoff
				+ i * ehdr->e_phentsize);

		if(phdr->p_type == PT_LOAD) {
			bool shared = false;
			if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
				// we cannot share this segment
			}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
				// TODO: share this segment
			}else{
				debug::panicLogger.log() << "Illegal combination of segment permissions"
						<< debug::Finish();
			}

			ASSERT(phdr->p_memsz > 0);
			
			// align virtual address and length to page size
			uintptr_t virt_offset = phdr->p_vaddr;
			virt_offset -= virt_offset % kPageSize;

			size_t virt_length = (phdr->p_vaddr + phdr->p_memsz) - virt_offset;
			if((virt_length % kPageSize) != 0)
				virt_length += kPageSize - virt_length % kPageSize;

			object->segments.push(UniqueSegment(phdr->p_type,
					phdr->p_flags, virt_offset, virt_length,
					phdr->p_vaddr - virt_offset, phdr->p_offset, phdr->p_filesz));
		} //FIXME: handle other phdrs
	}
	
	return object;
}

void runObject(Object *object, HelHandle space, uintptr_t base_address) {
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

			helAllocateMemory(segment.virtLength, &memory);

			void *map_pointer;
			helMapMemory(memory, kHelNullHandle, nullptr,
					segment.virtLength, kHelMapReadWrite, &map_pointer);
			memset(map_pointer, 0, segment.virtLength);
			memcpy((void *)((uintptr_t)map_pointer + segment.fileDisplacement),
					(void *)((uintptr_t)object->imagePtr + segment.fileOffset),
					segment.fileLength);
		}

		uint32_t map_flags = 0;
		if((base_segment->elfFlags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
			map_flags |= kHelMapReadWrite;
		}else if((base_segment->elfFlags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
			map_flags |= kHelMapReadExecute;
		}else{
			debug::panicLogger.log() << "Illegal combination of segment permissions"
					<< debug::Finish();
		}
		
		void *actual_ptr;
		helMapMemory(memory, space,
				(void *)(base_address + base_segment->virtOffset),
				base_segment->virtLength, map_flags, &actual_ptr);
	}
}

void onAccept(int64_t submit_id, HelHandle channel_handle) {
	infoLogger->log() << "Accept" << debug::Finish();
}

util::LazyInitializer<helx::EventHub> eventHub;
util::LazyInitializer<helx::Server> server;

int main() {
	infoLogger.initialize(infoSink);
	infoLogger->log() << "Entering ld-server" << debug::Finish();
	allocator.initialize(virtualAlloc);

	eventHub.initialize();

	// create a server and listen for requests
	HelHandle serve_handle, client_handle;
	helCreateServer(&serve_handle, &client_handle);

	server.initialize(serve_handle);
	server->accept(*eventHub, helx::AcceptCb::make<&onAccept>());
	
	// inform k_init that we are ready to server requests
	const char *path = "k_init";
	HelHandle channel_handle;
	helRdOpen(path, strlen(path), &channel_handle);

	helx::Channel channel(channel_handle);

//	HelHandle space;
//	helCreateSpace(&space);

//	Object *object = readObject("ld-init.so");
//	runObject(object, space, 0x40000000);

	infoLogger->log() << "ld-server initialized succesfully!" << debug::Finish();
	
	return 0;
}

