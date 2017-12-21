
#include <frigg/string.hpp>
#include <frigg/optional.hpp>

struct ObjectRepository;
struct Scope;
struct Loader;
struct SharedObject;

extern uint64_t rtsCounter;

enum class TlsModel {
	null,
	initial,
	dynamic
};

// --------------------------------------------------------
// ObjectRepository
// --------------------------------------------------------

struct ObjectRepository {
	ObjectRepository();

	ObjectRepository(const ObjectRepository &) = delete;

	ObjectRepository &operator= (const ObjectRepository &) = delete;

	// This is primarily used to create a SharedObject for the RTDL itself.
	SharedObject *injectObjectFromDts(frigg::StringView name,
			uintptr_t base_address, Elf64_Dyn *dynamic, uint64_t rts);

	// This is used to create a SharedObject for the executable that we want to link.
	SharedObject *injectObjectFromPhdrs(frigg::StringView name,
			void *phdr_pointer, size_t phdr_entry_size, size_t num_phdrs, void *entry_pointer,
			uint64_t rts);

	SharedObject *requestObjectWithName(frigg::StringView name, uint64_t rts);

	SharedObject *requestObjectAtPath(frigg::StringView path, uint64_t rts);

private:
	void _fetchFromPhdrs(SharedObject *object, void *phdr_pointer,
			size_t phdr_entry_size, size_t num_phdrs, void *entry_pointer);

	void _fetchFromFile(SharedObject *object, const char *file);

	void _parseDynamic(SharedObject *object);

	void _discoverDependencies(SharedObject *object, uint64_t rts);

	frigg::Hashmap<frigg::StringView, SharedObject *,
			frigg::DefaultHasher<frigg::StringView>, Allocator> _nameMap;
};

// FIXME: Do not depend on the initial universe everywhere.
extern frigg::LazyInitializer<ObjectRepository> initialRepository;

// --------------------------------------------------------
// SharedObject
// --------------------------------------------------------

struct SharedObject {
	SharedObject(const char *name, bool is_main_object,
			uint64_t object_rts);

	const char *name;
	bool isMainObject;
	uint64_t objectRts;

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

	bool symbolicResolution;
	bool haveStaticTls;

	// vector of dependencies
	frigg::Vector<SharedObject *, Allocator> dependencies;
	
	TlsModel tlsModel;
	size_t tlsOffset;
	
	uint64_t globalRts;
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
// ObjectSymbol
// --------------------------------------------------------

struct ObjectSymbol {
	ObjectSymbol(SharedObject *object, const Elf64_Sym *symbol);

	SharedObject *object() {
		return _object;
	}

	const Elf64_Sym *symbol() {
		return _symbol;
	}

	const char *getString();
	
	uintptr_t virtualAddress();

private:
	SharedObject *_object;
	const Elf64_Sym *_symbol;
};

// --------------------------------------------------------
// Scope
// --------------------------------------------------------

struct Scope {
	using ResolveFlags = uint32_t;
	static inline constexpr ResolveFlags resolveCopy = 1;

	static frigg::Optional<ObjectSymbol> resolveWholeScope(Scope *scope,
			frigg::StringView string, ResolveFlags flags);

	Scope();
	
	void appendObject(SharedObject *object);

	frigg::Optional<ObjectSymbol> resolveSymbol(ObjectSymbol r, ResolveFlags flags);

private:
	frigg::Vector<SharedObject *, Allocator> _objects;
};

// --------------------------------------------------------
// Loader
// --------------------------------------------------------

class Loader {
public:
	Loader(Scope *scope, TlsModel tls_model, uint64_t rts);

	void submitObject(SharedObject *object);

public:
	void linkObjects();

private:
	void _buildTlsMaps();

	void _processStaticRelocations(SharedObject *object);
	void _processLazyRelocations(SharedObject *object);
	void _processRela(SharedObject *object, Elf64_Rela *reloc);

public:
	void initObjects();

private:
	void _scheduleInit(SharedObject *object);

private:
	struct Token { };

	Scope *_globalScope;
	TlsModel _tlsModel;
	uint64_t _linkRts;

	frigg::Hashmap<SharedObject *, Token,
			frigg::DefaultHasher<SharedObject *>, Allocator> _linkSet;
	
	// Stores the same objects as _linkSet but in dependency-BFS order.
	frigg::LinkedList<SharedObject *, Allocator> _linkBfs;

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

