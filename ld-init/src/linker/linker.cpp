
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/initializer.hpp>
#include <frigg/support.hpp>
#include <frigg/debug.hpp>
#include <frigg/memory.hpp>
#include <frigg/libc.hpp>
#include <frigg/elf.hpp>

#include <frigg/tuple.hpp>
#include <frigg/vector.hpp>
#include <frigg/hashmap.hpp>
#include <frigg/linked.hpp>

#include <hel.h>
#include <hel-syscalls.h>

#include <frigg/glue-hel.hpp>

namespace debug = frigg::debug;
namespace util = frigg::util;
namespace memory = frigg::memory;

#include "linker.hpp"

uintptr_t libraryBase = 0x41000000;

// --------------------------------------------------------
// SharedObject
// --------------------------------------------------------

SharedObject::SharedObject() : baseAddress(0), loadScope(nullptr),
		dynamic(nullptr), globalOffsetTable(nullptr), entry(nullptr),
		hashTableOffset(0), symbolTableOffset(0), stringTableOffset(0),
		lazyRelocTableOffset(0), lazyTableSize(0),
		lazyExplicitAddend(false) { }

void processCopyRela(SharedObject *object, Elf64_Rela *reloc) {
	Elf64_Xword type = ELF64_R_TYPE(reloc->r_info);
	Elf64_Xword symbol_index = ELF64_R_SYM(reloc->r_info);
	ASSERT(type == R_X86_64_COPY);
	
	uintptr_t rel_addr = object->baseAddress + reloc->r_offset;
	
	auto symbol = (Elf64_Sym *)(object->baseAddress + object->symbolTableOffset
			+ symbol_index * sizeof(Elf64_Sym));
	ASSERT(symbol->st_name != 0);

	const char *symbol_str = (const char *)(object->baseAddress
			+ object->stringTableOffset + symbol->st_name);
	uintptr_t copy_addr = (uintptr_t)object->loadScope->resolveSymbol(symbol_str,
			object, Scope::kResolveCopy);
	ASSERT(copy_addr != 0);

	memcpy((void *)rel_addr, (void *)copy_addr, symbol->st_size);
}

void processCopyRelocations(SharedObject *object) {
	bool has_rela_offset = false, has_rela_length = false;
	uintptr_t rela_offset;
	size_t rela_length;

	for(size_t i = 0; object->dynamic[i].d_tag != DT_NULL; i++) {
		Elf64_Dyn *dynamic = &object->dynamic[i];
		
		switch(dynamic->d_tag) {
		case DT_RELA:
			rela_offset = dynamic->d_ptr;
			has_rela_offset = true;
			break;
		case DT_RELASZ:
			rela_length = dynamic->d_val;
			has_rela_length = true;
			break;
		case DT_RELAENT:
			ASSERT(dynamic->d_val == sizeof(Elf64_Rela));
			break;
		}
	}

	if(has_rela_offset && has_rela_length) {
		for(size_t offset = 0; offset < rela_length; offset += sizeof(Elf64_Rela)) {
			auto reloc = (Elf64_Rela *)(object->baseAddress + rela_offset + offset);
			processCopyRela(object, reloc);
		}
	}else{
		ASSERT(!has_rela_offset && !has_rela_length);
	}
}

// --------------------------------------------------------
// Scope
// --------------------------------------------------------

Scope::Scope() : objects(*allocator) { }

bool strEquals(const char *str1, const char *str2) {
	while(*str1 != 0 && *str2 != 0) {
		if(*str1++ != *str2++)
			return false;
	}
	if(*str1 != 0 || *str2 != 0)
		return false;
	return true;
}

// TODO: move this to some namespace or class?
void *resolveInObject(SharedObject *object, const char *resolve_str) {
	auto hash_table = (Elf64_Word *)(object->baseAddress + object->hashTableOffset);
	
	Elf64_Word num_buckets = hash_table[0];
	Elf64_Word num_chains = hash_table[1];

	for(size_t i = 0; i < num_chains; i++) {
		auto *symbol = (Elf64_Sym *)(object->baseAddress
				+ object->symbolTableOffset + i * sizeof(Elf64_Sym));
		uint8_t type = symbol->st_info & 0x0F;
		uint8_t bind = symbol->st_info >> 4;
		if(bind != STB_GLOBAL)
			continue; // TODO: support local and weak symbols
		if(symbol->st_shndx == SHN_UNDEF)
			continue;
		ASSERT(symbol->st_name != 0);

		const char *symbol_str = (const char *)(object->baseAddress
				+ object->stringTableOffset + symbol->st_name);
		//FIXME infoLogger->log() << (void *)object->baseAddress
		//		<< " " << (void *)object->stringTableOffset
		//		<< " " << (void *)symbol->st_name << " " << (void *)symbol_str << debug::Finish();
		if(strEquals(symbol_str, resolve_str))
			return (void *)(object->baseAddress + symbol->st_value);
	}
	
	return nullptr;
}

// TODO: let this return uintptr_t
void *Scope::resolveSymbol(const char *resolve_str,
		SharedObject *from_object, uint32_t flags) {
	for(size_t i = 0; i < objects.size(); i++) {
		if((flags & kResolveCopy) != 0 && objects[i] == from_object)
			continue;

		void *resolved = resolveInObject(objects[i], resolve_str);
		if(resolved != nullptr)
			return resolved;
	}

	return nullptr;
}


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
			uint32_t map_flags = 0;
			if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
				map_flags |= kHelMapReadWrite;
			}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
				map_flags |= kHelMapReadExecute;
			}else{
				debug::panicLogger.log() << "Illegal combination of segment permissions"
						<< debug::Finish();
			}

			HelHandle memory = loadSegment(image, object->baseAddress + phdr->p_vaddr,
					phdr->p_offset, phdr->p_memsz, phdr->p_filesz);
			mapSegment(memory, object->baseAddress + phdr->p_vaddr,
					phdr->p_memsz, map_flags);
		}else if(phdr->p_type == PT_DYNAMIC) {
			object->dynamic = (Elf64_Dyn *)(object->baseAddress + phdr->p_vaddr);
		} //FIXME: handle other phdrs
	}

	parseDynamic(object);
	p_processQueue.addBack(object);
	p_scope->objects.push(object);
}

void Loader::process() {
	while(!p_processQueue.empty()) {
		SharedObject *object = p_processQueue.front();
		object->loadScope = p_scope;
		infoLogger->log() << "process at " << (void *)object->baseAddress << debug::Finish();

		processDependencies(object);
		processStaticRelocations(object);
		processLazyRelocations(object);

		p_processQueue.removeFront();
	}
}

void Loader::parseDynamic(SharedObject *object) {
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
		case DT_SONAME: case DT_NEEDED: // we handle this later
		case DT_INIT: case DT_FINI:
		case DT_DEBUG:
		case DT_RELA: case DT_RELASZ: case DT_RELAENT: case DT_RELACOUNT:
		case DT_VERSYM:
		case DT_VERDEF: case DT_VERDEFNUM:
		case DT_VERNEED: case DT_VERNEEDNUM:
			break;
		default:
			debug::panicLogger.log() << "Unexpected dynamic entry "
					<< (void *)dynamic->d_tag << " in object" << debug::Finish();
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
		helMapMemory(library_handle, kHelNullHandle, nullptr, size,
				kHelMapReadOnly, &actual_pointer);
		
		auto library = memory::construct<SharedObject>(*allocator);
		library->baseAddress = libraryBase;
		// TODO: handle this dynamically
		libraryBase += 0x1000000; // assume 16 MiB per library
		loadFromImage(library, actual_pointer);

		helCloseDescriptor(library_handle);
	}
}

void Loader::processRela(SharedObject *object, Elf64_Rela *reloc) {
	Elf64_Xword type = ELF64_R_TYPE(reloc->r_info);
	Elf64_Xword symbol_index = ELF64_R_SYM(reloc->r_info);
	
	if(type == R_X86_64_COPY)
		return; // TODO: make sure this only occurs in executables

	uintptr_t rel_addr = object->baseAddress + reloc->r_offset;
	
	// resolve the symbol if there is a symbol
	uintptr_t symbol_addr = 0;
	if(symbol_index != 0) {
		auto symbol = (Elf64_Sym *)(object->baseAddress + object->symbolTableOffset
				+ symbol_index * sizeof(Elf64_Sym));
		ASSERT(symbol->st_name != 0);

		const char *symbol_str = (const char *)(object->baseAddress
				+ object->stringTableOffset + symbol->st_name);
		//FIXME infoLogger->log() << "Looking up " << symbol_str << debug::Finish();
		symbol_addr = (uintptr_t)object->loadScope->resolveSymbol(symbol_str, object, 0);
		if(symbol_addr == 0 && ELF64_ST_BIND(symbol->st_info) != STB_WEAK)
			debug::panicLogger.log() << "Unresolved static symbol "
					<< (const char *)symbol_str << debug::Finish();
	}

	switch(type) {
	case R_X86_64_64:
		*((uint64_t *)rel_addr) = symbol_addr + reloc->r_addend;
		//FIXME infoLogger->log() << "R_X86_64_64 at " << (void *)rel_addr
		//		<< " resolved to " << (void *)(symbol_addr + reloc->r_addend)
		//		<< debug::Finish();
		break;
	case R_X86_64_GLOB_DAT:
		*((uint64_t *)rel_addr) = symbol_addr;
		//FIXME infoLogger->log() << "R_X86_64_GLOB_DAT at " << (void *)rel_addr
		//		<< " resolved to " << (void *)(symbol_addr)
		//		<< debug::Finish();
		break;
	case R_X86_64_RELATIVE:
		*((uint64_t *)rel_addr) = object->baseAddress + reloc->r_addend;
		//FIXME infoLogger->log() << "R_X86_64_RELATIVE at " << (void *)rel_addr
		//		<< " resolved to " << (void *)(object->baseAddress + reloc->r_addend)
		//		<< debug::Finish();
		break;
	default:
		debug::panicLogger.log() << "Unexpected relocation type "
				<< (void *)type << debug::Finish();
	}
}

void Loader::processStaticRelocations(SharedObject *object) {
	bool has_rela_offset = false, has_rela_length = false;
	uintptr_t rela_offset;
	size_t rela_length;

	for(size_t i = 0; object->dynamic[i].d_tag != DT_NULL; i++) {
		Elf64_Dyn *dynamic = &object->dynamic[i];
		
		switch(dynamic->d_tag) {
		case DT_RELA:
			rela_offset = dynamic->d_ptr;
			has_rela_offset = true;
			break;
		case DT_RELASZ:
			rela_length = dynamic->d_val;
			has_rela_length = true;
			break;
		case DT_RELAENT:
			ASSERT(dynamic->d_val == sizeof(Elf64_Rela));
			break;
		}
	}

	if(has_rela_offset && has_rela_length) {
		for(size_t offset = 0; offset < rela_length; offset += sizeof(Elf64_Rela)) {
			auto reloc = (Elf64_Rela *)(object->baseAddress + rela_offset + offset);
			processRela(object, reloc);
		}
	}else{
		ASSERT(!has_rela_offset && !has_rela_length);
	}
}

void Loader::processLazyRelocations(SharedObject *object) {
	if(object->globalOffsetTable == nullptr) {
		ASSERT(object->lazyRelocTableOffset == 0);
		return;
	}

	object->globalOffsetTable[1] = object;
	object->globalOffsetTable[2] = (void *)&pltRelocateStub;
	
	// adjust the addresses of JUMP_SLOT relocations
	ASSERT(object->lazyExplicitAddend);
	for(size_t offset = 0; offset < object->lazyTableSize; offset += sizeof(Elf64_Rela)) {
		auto reloc = (Elf64_Rela *)(object->baseAddress + object->lazyRelocTableOffset + offset);
		Elf64_Xword type = ELF64_R_TYPE(reloc->r_info);
		Elf64_Xword symbol_index = ELF64_R_SYM(reloc->r_info);

		uintptr_t rel_addr = object->baseAddress + reloc->r_offset;

		ASSERT(type == R_X86_64_JUMP_SLOT);
		*((uint64_t *)rel_addr) += object->baseAddress;
	}
}

// --------------------------------------------------------
// Namespace scope functions
// --------------------------------------------------------

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

void mapSegment(HelHandle memory, uintptr_t address,
		size_t length, uint32_t map_flags) {
	ASSERT(length > 0);
	util::Tuple<uintptr_t, size_t> map = calcSegmentMap(address, length);

	void *actual_ptr;
	helMapMemory(memory, kHelNullHandle, (void *)map.get<0>(), map.get<1>(),
			map_flags, &actual_ptr);
	ASSERT(actual_ptr == (void *)map.get<0>());
}

