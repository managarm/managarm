
#include <frigg/string.hpp>
#include <frigg/optional.hpp>

struct Scope;

// --------------------------------------------------------
// SharedObject
// --------------------------------------------------------

struct SharedObject {
	enum TlsModel {
		kTlsNone,
		kTlsInitial,
		kTlsDynamic
	};

	SharedObject(const char *name, bool is_main_object);

	const char *name;
	bool isMainObject;

	// base address this shared object was loaded to
	uintptr_t baseAddress;

	Scope *loadScope;
	
	// pointers to the dynamic table, GOT and entry point
	Elf64_Dyn *dynamic;
	void **globalOffsetTable;
	void *entry;

	// TODO: read this from the PHDR
	size_t tlsSegmentSize, tlsAlignment, tlsImageSize;
	void *tlsImagePtr;
	
	// symbol and string table of this shared object
	uintptr_t hashTableOffset;
	uintptr_t symbolTableOffset;
	uintptr_t stringTableOffset;
	
	// save the lazy JUMP_SLOT relocation table
	uintptr_t lazyRelocTableOffset;
	size_t lazyTableSize;
	bool lazyExplicitAddend;

	// vector of dependencies
	frigg::Vector<SharedObject *, Allocator> dependencies;
	
	TlsModel tlsModel;
	size_t tlsOffset;

	bool onInitStack;
	bool wasInitialized;
};

void processCopyRelocations(SharedObject *object);

// --------------------------------------------------------
// RuntimeTlsMap
// --------------------------------------------------------

struct RuntimeTlsMap {
	RuntimeTlsMap();

	size_t initialSize;
	frigg::Vector<SharedObject *, Allocator> initialObjects;
};

extern frigg::LazyInitializer<RuntimeTlsMap> runtimeTlsMap;

void allocateTcb();

// --------------------------------------------------------
// SymbolRef
// --------------------------------------------------------

struct SymbolRef {
	SymbolRef(SharedObject *object, Elf64_Sym &symbol);

	const char *getString();
	
	uintptr_t virtualAddress();

	SharedObject *object;
	Elf64_Sym symbol;
};

// --------------------------------------------------------
// Scope
// --------------------------------------------------------

struct Scope {
	enum : uint32_t {
		kResolveCopy = 1
	};

	Scope();
	
	void appendObject(SharedObject *object);

	void buildScope(SharedObject *object);

	frigg::Optional<SymbolRef> resolveSymbol(SymbolRef r, uint32_t flags);

	frigg::Vector<SharedObject *, Allocator> objects;
};

// --------------------------------------------------------
// Loader
// --------------------------------------------------------

class Loader {
public:
	Loader(Scope *scope);
	
	void loadFromPhdr(SharedObject *object, void *phdr_pointer,
			size_t phdr_entry_size, size_t num_phdrs, void *entry_pointer);
	void loadFromFile(SharedObject *object, const char *file);

	void buildInitialTls();

	void linkObjects();

	void initObjects();

private:
	void parseDynamic(SharedObject *object);
	void processDependencies(SharedObject *object);
	void processStaticRelocations(SharedObject *object);
	void processLazyRelocations(SharedObject *object);
	
	void processRela(SharedObject *object, Elf64_Rela *reloc);

	Scope *p_scope;
	frigg::LinkedList<SharedObject *, Allocator> p_linkQueue;
	frigg::LinkedList<SharedObject *, Allocator> p_initQueue;

	// FIXME: move this to an own class
public:
	frigg::Hashmap<frigg::String<Allocator>, SharedObject *,
			frigg::DefaultHasher<frigg::String<Allocator>>, Allocator> p_allObjects;
};

// --------------------------------------------------------
// Namespace scope functions
// --------------------------------------------------------

extern "C" void pltRelocateStub() __attribute__ (( visibility("hidden") ));

extern HelHandle *fileTable;

// --------------------------------------------------------
// RTDL interface
// --------------------------------------------------------

void *rtdl_auxvector();

