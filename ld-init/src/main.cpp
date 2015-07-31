
#include "../../frigg/include/types.hpp"
#include "../../frigg/include/traits.hpp"
#include "../../frigg/include/initializer.hpp"
#include "../../frigg/include/support.hpp"
#include "../../frigg/include/debug.hpp"
#include "../../frigg/include/libc.hpp"
#include "../../frigg/include/elf.hpp"

#include "../../hel/include/hel.h"
#include "../../hel/include/hel-syscalls.h"

namespace debug = frigg::debug;
namespace util = frigg::util;

#define HIDDEN  __attribute__ ((visibility ("hidden")))
#define EXPORT  __attribute__ ((visibility ("default")))

extern HIDDEN void *_GLOBAL_OFFSET_TABLE_[];
extern HIDDEN Elf64_Dyn _DYNAMIC[];
extern "C" HIDDEN void pltRelocateStub();

class InfoSink {
public:
	void print(char c) {
		helLog(&c, 1);
	}

	void print(const char *str) {
		size_t length = 0;
		for(size_t i = 0; str[i] != 0; i++)
			length++;
		helLog(str, length);
	}
};

InfoSink infoSink;
typedef debug::DefaultLogger<InfoSink> InfoLogger;
util::LazyInitializer<InfoLogger> infoLogger;

void friggPrintCritical(char c) {
	infoSink.print(c);
}

void friggPrintCritical(const char *str) {
	infoSink.print(str);
}

void friggPanic() {
	helPanic("Abort", 5);
}

struct SharedObject {
	SharedObject();

	// base address this shared object was loaded to
	uintptr_t baseAddress;
	
	// pointers to the dynamic table, GOT and entry point
	Elf64_Dyn *dynamic;
	void **globalOffsetTable;
	void *entry;
	
	// symbol and string table of this shared object
	uintptr_t hashTableOffset;
	uintptr_t symbolTableOffset;
	uintptr_t stringTableOffset;
	
	// save the lazy JUMP_SLOT relocation table
	uintptr_t lazyRelocTableOffset;
	size_t lazyTableSize;
	bool lazyExplicitAddend;
};

SharedObject::SharedObject() : baseAddress(0),
		dynamic(nullptr), globalOffsetTable(nullptr), entry(nullptr),
		hashTableOffset(0), symbolTableOffset(0), stringTableOffset(0),
		lazyRelocTableOffset(0), lazyTableSize(0),
		lazyExplicitAddend(false) { }

util::LazyInitializer<SharedObject> interpreter;
util::LazyInitializer<SharedObject> executable;
util::LazyInitializer<SharedObject> library;

void unresolvedSymbol(const char *symbol_str) {
	debug::panicLogger.log() << "Unresolved symbol" << debug::Finish();
}

bool strEquals(const char *str1, const char *str2) {
	while(*str1 != 0 && *str2 != 0) {
		if(*str1++ != *str2++)
			return false;
	}
	if(*str1 != 0 || *str2 != 0)
		return false;
	return true;
}

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

		const char *symbol_str = (const char *)(object->baseAddress
				+ object->stringTableOffset + symbol->st_name);
		if(strEquals(symbol_str, resolve_str))
			return (void *)(object->baseAddress + symbol->st_value);
	}
	
	return nullptr;
}

void *symbolResolve(SharedObject *object, const char *resolve_str) {
	void *resolved = resolveInObject(object, resolve_str);
	if(resolved != nullptr)
		return resolved;
	return resolveInObject(library.get(), resolve_str);
}

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

void loadObject(SharedObject *object, void *image);

void processDynamic(SharedObject *object) {
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
		helMapMemory(library_handle, (void *)0x42000000, size, &actual_pointer);
		
		library.initialize();
		library->baseAddress = 0x43000000;
		loadObject(library.get(), actual_pointer);

		helCloseDescriptor(library_handle);
	}
	
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

void loadObject(SharedObject *object, void *image) {
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

	processDynamic(object);
}

extern "C" void *lazyRelocate(SharedObject *object, unsigned int rel_index) {
	infoLogger->log() << "lazyRelocate()" << debug::Finish();

	ASSERT(object->lazyExplicitAddend);
	auto reloc = (Elf64_Rela *)(object->baseAddress + object->lazyRelocTableOffset
			+ rel_index * sizeof(Elf64_Rela));
	Elf64_Xword type = reloc->r_info & 0xFFFFFFFF;
	Elf64_Xword symbol_index = reloc->r_info >> 32;

	ASSERT(type == R_X86_64_JUMP_SLOT);

	auto symbol = (Elf64_Sym *)(object->baseAddress + object->symbolTableOffset
			+ symbol_index * sizeof(Elf64_Sym));
	ASSERT(symbol->st_name != 0);

	const char *symbol_str = (const char *)(object->baseAddress
			+ object->stringTableOffset + symbol->st_name);

	void *pointer = symbolResolve(object, symbol_str);
	if(pointer == nullptr)
		unresolvedSymbol(symbol_str);

	infoLogger->log() << "Resolved lazy relocation!" << debug::Finish();
	*(void **)(object->baseAddress + reloc->r_offset) = pointer;
	return pointer;
}

extern "C" void *interpreterMain(HelHandle program_handle) {
	infoLogger.initialize(infoSink);
	infoLogger->log() << "Entering ld-init" << debug::Finish();
	
	interpreter.initialize();
	interpreter->baseAddress = (uintptr_t)_DYNAMIC
			- (uintptr_t)_GLOBAL_OFFSET_TABLE_[0];

	_GLOBAL_OFFSET_TABLE_[1] = interpreter.get();
	_GLOBAL_OFFSET_TABLE_[2] = (void *)&pltRelocateStub;
	
	for(size_t i = 0; _DYNAMIC[i].d_tag != DT_NULL; i++) {
		Elf64_Dyn *dynamic = &_DYNAMIC[i];
		switch(dynamic->d_tag) {
		// handle hash table, symbol table and string table
		case DT_HASH:
			interpreter->hashTableOffset = dynamic->d_ptr;
			break;
		case DT_STRTAB:
			interpreter->stringTableOffset = dynamic->d_ptr;
			break;
		case DT_STRSZ:
			break; // we don't need the size of the string table
		case DT_SYMTAB:
			interpreter->symbolTableOffset = dynamic->d_ptr;
			break;
		case DT_SYMENT:
			ASSERT(dynamic->d_val == sizeof(Elf64_Sym));
			break;
		default:
			ASSERT(!"Unexpected dynamic entry in program interpreter");
		}
	}

	size_t size;
	void *actual_pointer;
	helMemoryInfo(program_handle, &size);
	helMapMemory(program_handle, (void *)0x41000000, size, &actual_pointer);
	
	executable.initialize();
	loadObject(executable.get(), actual_pointer);
	
	helCloseDescriptor(program_handle);

	infoLogger->log() << "Leaving ld-init" << debug::Finish();
	return executable->entry;
}

