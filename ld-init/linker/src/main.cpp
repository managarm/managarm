
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/support.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/libc.hpp>
#include <frigg/elf.hpp>

#include <frigg/optional.hpp>
#include <frigg/tuple.hpp>
#include <frigg/vector.hpp>
#include <frigg/hashmap.hpp>
#include <frigg/linked.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include <frigg/glue-hel.hpp>

#include "linker.hpp"

#define HIDDEN  __attribute__ ((visibility ("hidden")))
#define EXPORT  __attribute__ ((visibility ("default")))

extern HIDDEN void *_GLOBAL_OFFSET_TABLE_[];
extern HIDDEN Elf64_Dyn _DYNAMIC[];

frigg::LazyInitializer<SharedObject> interpreter;
frigg::LazyInitializer<SharedObject> executable;
frigg::LazyInitializer<Scope> globalScope;
frigg::LazyInitializer<Loader> globalLoader;

frigg::LazyInitializer<RuntimeTlsMap> runtimeTlsMap;

extern "C" void *lazyRelocate(SharedObject *object, unsigned int rel_index) {
	assert(object->lazyExplicitAddend);
	auto reloc = (Elf64_Rela *)(object->baseAddress + object->lazyRelocTableOffset
			+ rel_index * sizeof(Elf64_Rela));
	Elf64_Xword type = ELF64_R_TYPE(reloc->r_info);
	Elf64_Xword symbol_index = ELF64_R_SYM(reloc->r_info);

	assert(type == R_X86_64_JUMP_SLOT);

	auto symbol = (Elf64_Sym *)(object->baseAddress + object->symbolTableOffset
			+ symbol_index * sizeof(Elf64_Sym));
	SymbolRef r(object, *symbol);
	frigg::Optional<SymbolRef> p = object->loadScope->resolveSymbol(r, 0);
	if(!p)
		frigg::panicLogger.log() << "Unresolved JUMP_SLOT symbol" << frigg::EndLog();

	//infoLogger->log() << "Lazy relocation to " << symbol_str
	//		<< " resolved to " << pointer << frigg::EndLog();
	
	*(uint64_t *)(object->baseAddress + reloc->r_offset) = p->virtualAddress();
	return (void *)p->virtualAddress();
}

frigg::LazyInitializer<helx::EventHub> eventHub;
frigg::LazyInitializer<helx::Pipe> serverPipe;

extern "C" void *interpreterMain(void *phdr_pointer,
		size_t phdr_entry_size, size_t phdr_count, void *entry_pointer) {
	infoLogger.initialize(infoSink);
	//infoLogger->log() << "Entering ld-init" << frigg::EndLog();
	allocator.initialize(virtualAlloc);
	runtimeTlsMap.initialize();

	// FIXME: read own SONAME
	interpreter.initialize("ld-init.so", false);
	interpreter->baseAddress = (uintptr_t)_DYNAMIC
			- (uintptr_t)_GLOBAL_OFFSET_TABLE_[0];
	interpreter->dynamic = _DYNAMIC;

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
			assert(dynamic->d_val == sizeof(Elf64_Sym));
			break;
		default:
			assert(!"Unexpected dynamic entry in program interpreter");
		}
	}

	eventHub.initialize(helx::EventHub::create());
	serverPipe.initialize();
	
	const char *path = "config/rtdl-server";
	HelHandle server_handle;
	HEL_CHECK(helRdOpen(path, strlen(path), &server_handle));
	
	int64_t async_id;
	HEL_CHECK(helSubmitConnect(server_handle, eventHub->getHandle(),
			kHelNoFunction, kHelNoObject, &async_id));
	HEL_CHECK(helCloseDescriptor(server_handle));

	HelError connect_error;
	eventHub->waitForConnect(async_id, connect_error, *serverPipe);
	HEL_CHECK(connect_error);
	
	globalScope.initialize();
	globalLoader.initialize(globalScope.get());
	globalLoader->p_allObjects.insert(frigg::String<Allocator>(*allocator, interpreter->name),
			interpreter.get());

	executable.initialize("(executable)", true);
	// TODO: support non-zero base addresses?
	globalLoader->loadFromPhdr(executable.get(), phdr_pointer,
			phdr_entry_size, phdr_count, entry_pointer);

	globalLoader->buildInitialTls();
	globalScope->buildScope(executable.get());

	globalLoader->linkObjects();
	processCopyRelocations(executable.get());
	globalLoader->initObjects();

//	infoLogger->log() << "Leaving ld-init" << frigg::EndLog();
	return executable->entry;
}

// the layout of this structure is dictated by the ABI
struct TlsEntry {
	SharedObject *object;
	uint64_t offset;
};

static_assert(sizeof(TlsEntry) == 16, "Bad TlsEntry size");

extern "C" __attribute__ (( visibility("default") ))
void *__tls_get_addr(TlsEntry *entry) {
	assert(entry->object->tlsModel == SharedObject::kTlsInitial);
	
	frigg::infoLogger.log() << "__tls_get_addr(" << entry->object->name
			<< ", " << entry->offset << ")" << frigg::EndLog();
	
	char *tp;
	asm ( "mov %%fs:(0), %0" : "=r" (tp) );
	return tp + entry->object->tlsOffset + entry->offset;
}

