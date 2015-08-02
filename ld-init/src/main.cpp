
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

#define HIDDEN  __attribute__ ((visibility ("hidden")))
#define EXPORT  __attribute__ ((visibility ("default")))

extern HIDDEN void *_GLOBAL_OFFSET_TABLE_[];
extern HIDDEN Elf64_Dyn _DYNAMIC[];

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
	__builtin_unreachable();
}

uintptr_t VirtualAlloc::map(size_t length) {
	ASSERT((length % 0x1000) == 0);

	HelHandle memory;
	void *actual_ptr;
	helAllocateMemory(length, &memory);
	helMapMemory(memory, nullptr, length, &actual_ptr);
	return (uintptr_t)actual_ptr;
}

void VirtualAlloc::unmap(uintptr_t address, size_t length) {

}

typedef memory::DebugAllocator<VirtualAlloc> Allocator;
VirtualAlloc virtualAlloc;
util::LazyInitializer<Allocator> allocator;

util::LazyInitializer<SharedObject> interpreter;
util::LazyInitializer<SharedObject> executable;
util::LazyInitializer<Scope> globalScope;
util::LazyInitializer<Loader> globalLoader;

typedef util::Hashmap<const char *, SharedObject *,
		util::CStringHasher, Allocator> ObjectHashmap;
util::LazyInitializer<ObjectHashmap> allObjects;

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

void *resolveInScope(Scope *scope, const char *resolve_str) {
	for(size_t i = 0; i < scope->objects.size(); i++) {
		void *resolved = resolveInObject(scope->objects[i], resolve_str);
		if(resolved != nullptr)
			return resolved;
	}

	return nullptr;
}

void *symbolResolve(SharedObject *object, const char *resolve_str) {
	return resolveInScope(globalScope.get(), resolve_str);
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

	allocator.initialize(virtualAlloc);
	
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
	helMapMemory(program_handle, nullptr, size, &actual_pointer);

	executable.initialize();

	globalScope.initialize();
	globalLoader.initialize(globalScope.get());
	globalLoader->loadFromImage(executable.get(), actual_pointer);
	globalLoader->process();
	
	helCloseDescriptor(program_handle);

	infoLogger->log() << "Leaving ld-init" << debug::Finish();
	return executable->entry;
}

