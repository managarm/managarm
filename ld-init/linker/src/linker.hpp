
#include <frigg/string.hpp>
#include <frigg/optional.hpp>

struct LinkUniverse;
struct Scope;
struct Loader;
struct SharedObject;

// --------------------------------------------------------
// LinkUniverse
// --------------------------------------------------------

struct LinkUniverse {
	LinkUniverse();

	LinkUniverse(const LinkUniverse &) = delete;

	LinkUniverse &operator= (const LinkUniverse &) = delete;

	// This is primarily used to create a SharedObject for the RTDL itself.
	SharedObject *injectObjectFromDts(frigg::StringView name,
			uintptr_t base_address, Elf64_Dyn *dynamic);

	// This is used to create a SharedObject for the executable that we want to link.
	SharedObject *injectObjectFromPhdrs(frigg::StringView name,
			void *phdr_pointer, size_t phdr_entry_size, size_t num_phdrs, void *entry_pointer);

	SharedObject *requestObjectWithName(frigg::StringView name);

	SharedObject *requestObjectAtPath(frigg::StringView path);

private:
	void _fetchFromPhdrs(SharedObject *object, void *phdr_pointer,
			size_t phdr_entry_size, size_t num_phdrs, void *entry_pointer);

	void _fetchFromFile(SharedObject *object, const char *file);

	void _parseDynamic(SharedObject *object);

	void _discoverDependencies(SharedObject *object);

	frigg::Hashmap<frigg::StringView, SharedObject *,
			frigg::DefaultHasher<frigg::StringView>, Allocator> _nameMap;
};

// FIXME: Do not depend on the initial universe everywhere.
extern frigg::LazyInitializer<LinkUniverse> initialUniverse;

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
	
	bool wasLinked;

	bool scheduledForInit;
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

	void linkObject(SharedObject *object);

private:
public:
	void buildInitialTls();

	void linkObjects();
private:
	void processStaticRelocations(SharedObject *object);
	void processLazyRelocations(SharedObject *object);
	void processRela(SharedObject *object, Elf64_Rela *reloc);

public:
	void initObjects();
private:
	struct Token { };

	void _scheduleInit(SharedObject *object);
	
	Scope *p_scope;

	frigg::Hashmap<SharedObject *, Token,
			frigg::DefaultHasher<SharedObject *>, Allocator> _linkObjects;

	frigg::LinkedList<SharedObject *, Allocator> _linkQueue;
	frigg::LinkedList<SharedObject *, Allocator> _initQueue;

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

