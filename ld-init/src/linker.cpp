
#include "../../frigg/include/types.hpp"
#include "../../frigg/include/traits.hpp"
#include "../../frigg/include/initializer.hpp"
#include "../../frigg/include/support.hpp"
#include "../../frigg/include/debug.hpp"
#include "../../frigg/include/memory.hpp"
#include "../../frigg/include/libc.hpp"
#include "../../frigg/include/elf.hpp"

#include "../../frigg/include/vector.hpp"
#include "../../frigg/include/hashmap.hpp"
#include "../../frigg/include/linked.hpp"

#include "../../hel/include/hel.h"
#include "../../hel/include/hel-syscalls.h"

namespace debug = frigg::debug;
namespace util = frigg::util;
namespace memory = frigg::memory;

#include "linker.hpp"

uintptr_t libraryBase = 0x41000000;

// --------------------------------------------------------
// SharedObject
// --------------------------------------------------------

SharedObject::SharedObject() : baseAddress(0),
		dynamic(nullptr), globalOffsetTable(nullptr), entry(nullptr),
		hashTableOffset(0), symbolTableOffset(0), stringTableOffset(0),
		lazyRelocTableOffset(0), lazyTableSize(0),
		lazyExplicitAddend(false) { }

// --------------------------------------------------------
// Scope
// --------------------------------------------------------
Scope::Scope() : objects(*allocator) { }

// --------------------------------------------------------
// Loader
// --------------------------------------------------------

Loader::Loader(Scope *scope)
: p_scope(scope), p_processQueue(*allocator) { }

void Loader::loadFromImage(SharedObject *object, void *image) {
	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)image;
	ASSERT(ehdr->e_ident[0] == 0x7F
			&& ehdr->e_ident[1] == 'E'
			&& ehdr->e_ident[2] == 'L'
			&& ehdr->e_ident[3] == 'F');
	ASSERT(ehdr->e_type == ET_EXEC || ehdr->e_type == ET_DYN);
	
	object->entry = (void *)(object->baseAddress + ehdr->e_entry);

	for(int i = 0; i < ehdr->e_phnum; i++) {
		auto phdr = (Elf64_Phdr *)((uintptr_t)image + ehdr->e_phoff
				+ i * ehdr->e_phentsize);

		if(phdr->p_type == PT_LOAD) {
			loadSegment(image, object->baseAddress + phdr->p_vaddr,
					phdr->p_offset, phdr->p_memsz, phdr->p_filesz);
		}else if(phdr->p_type == PT_DYNAMIC) {
			object->dynamic = (Elf64_Dyn *)(object->baseAddress + phdr->p_vaddr);
		} //FIXME: handle other phdrs
	}

	p_processQueue.addBack(object);
}

void Loader::process() {
	while(!p_processQueue.empty()) {
		SharedObject *object = p_processQueue.front();

		processDynamic(object);
		processDependencies(object);
		processLazyRelocations(object);
		p_scope->objects.push(object);

		p_processQueue.removeFront();
	}
}

void Loader::processDynamic(SharedObject *object) {
	ASSERT(object->dynamic != nullptr);

	for(size_t i = 0; object->dynamic[i].d_tag != DT_NULL; i++) {
		Elf64_Dyn *dynamic = &object->dynamic[i];
		switch(dynamic->d_tag) {
		// handle hash table, symbol table and string table
		case DT_HASH:
			object->hashTableOffset = dynamic->d_ptr;
			break;
		case DT_STRTAB:
			object->stringTableOffset = dynamic->d_ptr;
			break;
		case DT_STRSZ:
			break; // we don't need the size of the string table
		case DT_SYMTAB:
			object->symbolTableOffset = dynamic->d_ptr;
			break;
		case DT_SYMENT:
			ASSERT(dynamic->d_val == sizeof(Elf64_Sym));
			break;
		// handle lazy relocation table
		case DT_PLTGOT:
			object->globalOffsetTable = (void **)(object->baseAddress
					+ dynamic->d_ptr);
			break;
		case DT_JMPREL:
			object->lazyRelocTableOffset = dynamic->d_ptr;
			break;
		case DT_PLTRELSZ:
			object->lazyTableSize = dynamic->d_val;
			break;
		case DT_PLTREL:
			if(dynamic->d_val == DT_RELA) {
				object->lazyExplicitAddend = true;
			}else{
				ASSERT(dynamic->d_val == DT_REL);
			}
			break;
		// ignore unimportant tags
		case DT_NEEDED: // we handle this later
		case DT_INIT:
		case DT_FINI:
		case DT_DEBUG:
			break;
		default:
			ASSERT(!"Unexpected dynamic entry in object");
		}
	}
}

void Loader::processDependencies(SharedObject *object) {
	// load required dynamic libraries
	for(size_t i = 0; object->dynamic[i].d_tag != DT_NULL; i++) {
		Elf64_Dyn *dynamic = &object->dynamic[i];
		if(dynamic->d_tag != DT_NEEDED)
			continue;

		const char *library_str = (const char *)(object->baseAddress
				+ object->stringTableOffset + dynamic->d_val);

		HelHandle library_handle;
		helRdOpen(library_str, strlen(library_str), &library_handle);

		size_t size;
		void *actual_pointer;
		helMemoryInfo(library_handle, &size);
		helMapMemory(library_handle, nullptr, size, &actual_pointer);
		
		auto library = memory::construct<SharedObject>(*allocator);
		library->baseAddress = libraryBase;
		// TODO: handle this dynamically
		libraryBase += 0x1000000; // assume 16 MiB per library
		loadFromImage(library, actual_pointer);

		helCloseDescriptor(library_handle);
	}
}

void Loader::processLazyRelocations(SharedObject *object) {
	if(object->globalOffsetTable != nullptr) {
		object->globalOffsetTable[1] = object;
		object->globalOffsetTable[2] = (void *)&pltRelocateStub;
		
		// adjust the addresses of JUMP_SLOT relocations
		ASSERT(object->lazyExplicitAddend);
		for(size_t offset = 0; offset < object->lazyTableSize;
				offset += sizeof(Elf64_Rela)) {
			auto reloc = (Elf64_Rela *)(object->baseAddress
					+ object->lazyRelocTableOffset + offset);
			Elf64_Xword type = reloc->r_info & 0xFF;
			Elf64_Xword symbol_index = reloc->r_info >> 8;

			ASSERT(type == R_X86_64_JUMP_SLOT);
			*(Elf64_Addr *)(object->baseAddress + reloc->r_offset)
					+= object->baseAddress;
		}
	}else{
		ASSERT(object->lazyRelocTableOffset == 0);
	}
}

// --------------------------------------------------------
// Namespace scope functions
// --------------------------------------------------------

void loadSegment(void *image, uintptr_t address, uintptr_t file_offset,
		size_t mem_length, size_t file_length) {
	uintptr_t limit = address + mem_length;
	if(address == limit)
		return;
	
	size_t page_size = 0x1000;
	uintptr_t map_page = address / page_size;
	uintptr_t num_pages = (limit / page_size) - map_page;
	if(limit % page_size != 0)
		num_pages++;
	
	uintptr_t map_address = map_page * page_size;
	uintptr_t map_length = num_pages * page_size;

	HelHandle memory;
	helAllocateMemory(num_pages * page_size, &memory);

	void *actual_ptr;
	helMapMemory(memory, (void *)map_address, map_length, &actual_ptr);
	ASSERT(actual_ptr == (void *)map_address);
	
	uintptr_t image_offset = (uintptr_t)image + file_offset;
	memset((void *)map_address, 0, map_length);
	memcpy((void *)address, (void *)image_offset, file_length);

	helCloseDescriptor(memory);
}

