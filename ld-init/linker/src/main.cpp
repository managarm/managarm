
#include <frigg/cxx-support.hpp>
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

HelHandle *fileTable;

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
		frigg::panicLogger() << "Unresolved JUMP_SLOT symbol" << frigg::endLog;

	//frigg::infoLogger() << "Lazy relocation to " << symbol_str
	//		<< " resolved to " << pointer << frigg::endLog;
	
	*(uint64_t *)(object->baseAddress + reloc->r_offset) = p->virtualAddress();
	return (void *)p->virtualAddress();
}

void *auxiliaryPtr;

extern "C" [[ gnu::visibility("default") ]] void *__rtdl_auxvector() {
	return auxiliaryPtr;
}

extern "C" [[ gnu::visibility("default") ]] void __rtdl_setupTcb() {
	allocateTcb();
}

template<typename T>
T loadItem(char *&sp) {
	T item;
	memcpy(&item, sp, sizeof(T));
	sp += sizeof(T);
	return item;
}

extern "C" void *interpreterMain(char *sp) {
	frigg::infoLogger() << "Entering ld-init" << frigg::endLog;
	auxiliaryPtr = sp;
	allocator.initialize(virtualAlloc);
	runtimeTlsMap.initialize();
	
	HelError error;
	asm volatile ("syscall" : "=D"(error), "=S"(fileTable) : "0"(kHelCallSuper + 1)
			: "rbx", "rcx", "r11");
	HEL_CHECK(error);

	// Parse the RTLDs own dynamic section.
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

	// Parse the auxiliary vector.
	enum {
		// this value is not part of the ABI
		AT_ILLEGAL = -1,

		AT_NULL = 0,
		AT_PHDR = 3,
		AT_PHENT = 4,
		AT_PHNUM = 5,
		AT_ENTRY = 9,
		
		AT_XPIPE = 0x1000,
		AT_OPENFILES = 0x1001,
		AT_MBUS_SERVER = 0x1103
	};

	struct Auxiliary {
		Auxiliary()
		: type(AT_ILLEGAL) { }

		int type;
		union {
			long longValue;
			void *pointerValue;
		};
	};

	void *phdr_pointer;
	size_t phdr_entry_size;
	size_t phdr_count;
	void *entry_pointer;
	
	while(true) {
		auto aux = loadItem<Auxiliary>(sp);
		if(aux.type == AT_NULL)
			break;
		
		switch(aux.type) {
			case AT_PHDR: phdr_pointer = aux.pointerValue; break;
			case AT_PHENT: phdr_entry_size = aux.longValue; break;
			case AT_PHNUM: phdr_count = aux.longValue; break;
			case AT_ENTRY: entry_pointer = aux.pointerValue; break;
			case AT_XPIPE:
			case AT_OPENFILES:
			case AT_MBUS_SERVER:
				// ignore these auxiliary vector entries.
				break;
		default:
			frigg::panicLogger() << "Unexpected auxiliary item type "
					<< aux.type << frigg::endLog;
		}
	}

	// perform the initial dynamic linking
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
	allocateTcb();
	globalLoader->initObjects();

	frigg::infoLogger() << "Leaving ld-init" << frigg::endLog;
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
	
//	frigg::infoLogger() << "__tls_get_addr(" << entry->object->name
//			<< ", " << entry->offset << ")" << frigg::endLog;
	
	char *tp;
	asm ( "mov %%fs:(0), %0" : "=r" (tp) );
	return tp + entry->object->tlsOffset + entry->offset;
}

