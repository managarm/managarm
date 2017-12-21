
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

static constexpr bool logEntryExit = false;

extern HIDDEN void *_GLOBAL_OFFSET_TABLE_[];
extern HIDDEN Elf64_Dyn _DYNAMIC[];

frigg::LazyInitializer<ObjectRepository> initialRepository;
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
	ObjectSymbol r(object, symbol);
	frigg::Optional<ObjectSymbol> p = object->loadScope->resolveSymbol(r, 0);
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
	if(logEntryExit)
		frigg::infoLogger() << "Entering ld-init" << frigg::endLog;
	auxiliaryPtr = sp;
	allocator.initialize(virtualAlloc);
	runtimeTlsMap.initialize();
	
	HelError error;
	asm volatile ("syscall" : "=D"(error), "=S"(fileTable) : "0"(kHelCallSuper + 1)
			: "rbx", "rcx", "r11");
	HEL_CHECK(error);

	// TODO: Use a fake PLT stub that reports an error message?
	_GLOBAL_OFFSET_TABLE_[1] = 0;
	_GLOBAL_OFFSET_TABLE_[2] = 0;
	
	// Validate our own dynamic section.
	// Here, we make sure that the dynamic linker does not need relocations itself.
	for(size_t i = 0; _DYNAMIC[i].d_tag != DT_NULL; i++) {
		Elf64_Dyn *dynamic = &_DYNAMIC[i];
		switch(dynamic->d_tag) {
		case DT_HASH:
		case DT_STRTAB:
		case DT_STRSZ:
		case DT_SYMTAB:
		case DT_SYMENT:
			continue;
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
	initialRepository.initialize();

	globalScope.initialize();

	// FIXME: read own SONAME
	initialRepository->injectObjectFromDts("ld-init.so", (uintptr_t)_DYNAMIC
			- (uintptr_t)_GLOBAL_OFFSET_TABLE_[0], _DYNAMIC, 1);
	// TODO: support non-zero base addresses?
	auto executable = initialRepository->injectObjectFromPhdrs("(executable)", phdr_pointer,
			phdr_entry_size, phdr_count, entry_pointer, 1);

	Loader linker{globalScope.get(), TlsModel::initial, 1};
	linker.submitObject(executable);
	linker.linkObjects();
	allocateTcb();
	linker.initObjects();

	if(logEntryExit)
		frigg::infoLogger() << "Leaving ld-init" << frigg::endLog;
	return executable->entry;
}

// the layout of this structure is dictated by the ABI
struct __abi_tls_entry {
	SharedObject *object;
	uint64_t offset;
};

static_assert(sizeof(__abi_tls_entry) == 16, "Bad __abi_tls_entry size");

extern "C" __attribute__ (( visibility("default") ))
void *__dlapi_get_tls(struct __abi_tls_entry *entry) {
	// TODO: Thread-safety!
	assert(entry->object->tlsModel == TlsModel::initial);
	
//	frigg::infoLogger() << "__tls_get_addr(" << entry->object->name
//			<< ", " << entry->offset << ")" << frigg::endLog;
	
	char *tp;
	asm ( "mov %%fs:(0), %0" : "=r" (tp) );
	return tp + entry->object->tlsOffset + entry->offset;
}

extern "C" [[gnu::visibility("default")]]
void *__dlapi_open(const char *file) {
	// TODO: Thread-safety!
	frigg::infoLogger() << "__dlapi_open(" << file << ")" << frigg::endLog;
	auto rts = rtsCounter++;

	SharedObject *object;
	if(frigg::StringView(file).findFirst('/') == size_t(-1)) {
		object = initialRepository->requestObjectWithName(file, rts);
	}else{
		frigg::panicLogger() << "__dlapi_open(): Support loading DSOs from paths" << frigg::endLog;
	}

	Loader linker{globalScope.get(), TlsModel::dynamic, rts};
	linker.submitObject(object);
	linker.linkObjects();
	linker.initObjects();

	return object;
}

extern "C" [[gnu::visibility("default")]]
void *__dlapi_resolve(void *handle, const char *string) {
	frigg::infoLogger() << "__dlapi_resolve(" << string << ")" << frigg::endLog;

	assert(!handle);
	auto target = Scope::resolveWholeScope(globalScope.get(), string, 0);
	assert(target);
	return reinterpret_cast<void *>(target->virtualAddress());
}

