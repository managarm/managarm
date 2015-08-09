
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

#define HIDDEN  __attribute__ ((visibility ("hidden")))
#define EXPORT  __attribute__ ((visibility ("default")))

extern HIDDEN void *_GLOBAL_OFFSET_TABLE_[];
extern HIDDEN Elf64_Dyn _DYNAMIC[];

util::LazyInitializer<SharedObject> interpreter;
util::LazyInitializer<SharedObject> executable;
util::LazyInitializer<Scope> globalScope;
util::LazyInitializer<Loader> globalLoader;

typedef util::Hashmap<const char *, SharedObject *,
		util::CStringHasher, Allocator> ObjectHashmap;
util::LazyInitializer<ObjectHashmap> allObjects;

extern "C" void *lazyRelocate(SharedObject *object, unsigned int rel_index) {
	ASSERT(object->lazyExplicitAddend);
	auto reloc = (Elf64_Rela *)(object->baseAddress + object->lazyRelocTableOffset
			+ rel_index * sizeof(Elf64_Rela));
	Elf64_Xword type = ELF64_R_TYPE(reloc->r_info);
	Elf64_Xword symbol_index = ELF64_R_SYM(reloc->r_info);

	ASSERT(type == R_X86_64_JUMP_SLOT);

	auto symbol = (Elf64_Sym *)(object->baseAddress + object->symbolTableOffset
			+ symbol_index * sizeof(Elf64_Sym));
	ASSERT(symbol->st_name != 0);

	const char *symbol_str = (const char *)(object->baseAddress
			+ object->stringTableOffset + symbol->st_name);

	void *pointer = globalScope->resolveSymbol(symbol_str, object, 0);
	if(pointer == nullptr)
		debug::panicLogger.log() << "Unresolved lazy symbol" << debug::Finish();

	//FIXME infoLogger->log() << "Lazy relocation to " << symbol_str
	//		<< " resolved to " << pointer << debug::Finish();

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
	helMapMemory(program_handle, kHelNullHandle, nullptr, size,
			kHelMapReadOnly, &actual_pointer);

	executable.initialize();

	globalScope.initialize();
	globalLoader.initialize(globalScope.get());
	globalLoader->loadFromImage(executable.get(), actual_pointer);
	globalLoader->process();
	
	helCloseDescriptor(program_handle);

	processCopyRelocations(executable.get());

	infoLogger->log() << "Leaving ld-init" << debug::Finish();
	return executable->entry;
}

