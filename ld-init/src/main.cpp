
#include "../../frigg/include/types.hpp"
#include "../../frigg/include/utils.hpp"
#include "../../frigg/include/initializer.hpp"
#include "../../frigg/include/debug.hpp"
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

// this function definition ensures that the program interpreter
// always contains a PLT and GOT
void EXPORT __ensurePltGot() {
	infoLogger->log() << "Linked!" << debug::Finish();
}

struct SharedObject {
	// base address this shared object was loaded to
	uintptr_t baseAddress;
	
	// symbol and string table of this shared object
	uintptr_t hashTableOffset;
	uintptr_t symbolTableOffset;
	uintptr_t stringTableOffset;
	size_t symbolEntrySize;
	
	// save the lazy JUMP_SLOT relocation table
	uintptr_t lazyRelocTableOffset;
	size_t lazyTableSize;
	bool lazyExplicitAddend;
};

SharedObject interpreter;

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
				+ object->symbolTableOffset + i * object->symbolEntrySize);
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
	return resolveInObject(object, resolve_str);
}

extern "C" void *memcpy(void *dest, const void *src, size_t n) {
	for(size_t i = 0; i < n; i++)
		((char *)dest)[i] = ((const char *)src)[i];
	return dest;
}
extern "C" void *memset(void *dest, int byte, size_t count) {
	for(size_t i = 0; i < count; i++)
		((char *)dest)[i] = (char)byte;
	return dest;
}

void *loadExecutable(void *image) {
	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)image;
	ASSERT(ehdr->e_ident[0] == 0x7F
			&& ehdr->e_ident[1] == 'E'
			&& ehdr->e_ident[2] == 'L'
			&& ehdr->e_ident[3] == 'F');
	ASSERT(ehdr->e_type == ET_EXEC);

	infoLogger->log() << "Loading executable" << debug::Finish();
	
	for(int i = 0; i < ehdr->e_phnum; i++) {
		auto phdr = (Elf64_Phdr *)((uintptr_t)image + ehdr->e_phoff
				+ i * ehdr->e_phentsize);

		if(phdr->p_type != PT_LOAD)
			continue;

		uintptr_t bottom = phdr->p_vaddr;
		uintptr_t top = phdr->p_vaddr + phdr->p_memsz;

		if(bottom == top)
			continue;
		
		size_t page_size = 0x1000;
		uintptr_t bottom_page = bottom / page_size;
		uintptr_t num_pages = (top / page_size) - bottom_page;
		if(top % page_size != 0)
			num_pages++;
		
		uintptr_t map_bottom = bottom_page * page_size;
		uintptr_t map_length = num_pages * page_size;

		HelHandle memory;
		helAllocateMemory(num_pages * page_size, &memory);

		void *actual_ptr;
		helMapMemory(memory, (void *)map_bottom, map_length, &actual_ptr);
		ASSERT(actual_ptr == (void *)map_bottom);
		
		uintptr_t image_offset = (uintptr_t)image + phdr->p_offset;
		memset((void *)map_bottom, 0, map_length);
		memcpy((void *)bottom, (void *)image_offset, phdr->p_filesz);
	}

	return (void *)ehdr->e_entry;
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
			+ symbol_index * object->symbolEntrySize);
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
	infoLogger->log() << "Entering ld-init" << debug::Finish();

	interpreter.baseAddress = (uintptr_t)_DYNAMIC
			- (uintptr_t)_GLOBAL_OFFSET_TABLE_[0];

	_GLOBAL_OFFSET_TABLE_[1] = &interpreter;
	_GLOBAL_OFFSET_TABLE_[2] = (void *)&pltRelocateStub;
	
	for(size_t i = 0; _DYNAMIC[i].d_tag != DT_NULL; i++) {
		Elf64_Dyn *dynamic = &_DYNAMIC[i];
		switch(dynamic->d_tag) {
		// handle hash table, symbol table and string table
		case DT_HASH:
			interpreter.hashTableOffset = dynamic->d_ptr;
			break;
		case DT_STRTAB:
			interpreter.stringTableOffset = dynamic->d_ptr;
			break;
		case DT_STRSZ:
			break; // we don't need the size of the string table
		case DT_SYMTAB:
			interpreter.symbolTableOffset = dynamic->d_ptr;
			break;
		case DT_SYMENT:
			interpreter.symbolEntrySize = dynamic->d_val;
			break;
		// handle lazy relocation table
		case DT_PLTGOT:
			ASSERT(interpreter.baseAddress + dynamic->d_ptr
					== (uintptr_t)_GLOBAL_OFFSET_TABLE_);
			break;
		case DT_JMPREL:
			interpreter.lazyRelocTableOffset = dynamic->d_ptr;
			break;
		case DT_PLTRELSZ:
			interpreter.lazyTableSize = dynamic->d_val;
			break;
		case DT_PLTREL:
			if(dynamic->d_val == DT_RELA) {
				interpreter.lazyExplicitAddend = true;
			}else{
				ASSERT(dynamic->d_val == DT_REL);
			}
			break;
		default:
			ASSERT(!"Unexpected dynamic entry in program interpreter");
		}
	}
	
	// adjust the addresses of JUMP_SLOT relocations
	ASSERT(interpreter.lazyExplicitAddend);
	for(size_t offset = 0; offset < interpreter.lazyTableSize;
			offset += sizeof(Elf64_Rela)) {
		auto reloc = (Elf64_Rela *)(interpreter.baseAddress
				+ interpreter.lazyRelocTableOffset + offset);
		Elf64_Xword type = reloc->r_info & 0xFF;
		Elf64_Xword symbol_index = reloc->r_info >> 8;

		ASSERT(type == R_X86_64_JUMP_SLOT);
		*(Elf64_Addr *)(interpreter.baseAddress + reloc->r_offset)
				+= interpreter.baseAddress;
	}

	__ensurePltGot();
	
	size_t size;
	void *actual_pointer;
	helMemoryInfo(program_handle, &size);
	helMapMemory(program_handle, (void *)0x41000000, size, &actual_pointer);
	void *entry = loadExecutable(actual_pointer);

	infoLogger->log() << "Leaving ld-init" << debug::Finish();
	return entry;
}

