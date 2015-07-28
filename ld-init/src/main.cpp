
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

void doLog(const char *message) {
	size_t length = 0;
	for(size_t i = 0; message[i] != 0; i++)
		length++;
	helLog(message, length);
}

void doPanic(const char *message) {
	size_t length = 0;
	for(size_t i = 0; message[i] != 0; i++)
		length++;
	helPanic(message, length);
}

namespace frigg {
namespace debug {

void assertionFail(const char *assertion) {
	doLog("Assertion failed!\n");
	doPanic(assertion);
}

} } // namespace frigg::debug

// this function definition ensures that the program interpreter
// always contains a PLT and GOT
void EXPORT __ensurePltGot() {
	doLog("Linked!\n");
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
	doPanic("Unresolved symbol");
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

extern "C" void *lazyRelocate(SharedObject *object, unsigned int rel_index) {
	doLog("lazyRelocate()\n");
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

	doLog("found symbol!\n");
	*(void **)(object->baseAddress + reloc->r_offset) = pointer;
	return pointer;
}

extern "C" void _start(uintptr_t base_addr) {
	interpreter.baseAddress = base_addr;

	doLog("Enter ld-init.so\n");
	ASSERT(interpreter.baseAddress
			+ (uintptr_t)_GLOBAL_OFFSET_TABLE_[0] == (uintptr_t)_DYNAMIC);
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
	doLog("Finished reading dynamic section\n");
	
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

	doLog("Leave ld-init.so\n");
	helExitThisThread();
	doPanic("Could not exit the thread");
}

