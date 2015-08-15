
struct Scope;

// --------------------------------------------------------
// SharedObject
// --------------------------------------------------------

struct SharedObject {
	SharedObject();

	// base address this shared object was loaded to
	uintptr_t baseAddress;

	Scope *loadScope;
	
	// pointers to the dynamic table, GOT and entry point
	Elf64_Dyn *dynamic;
	void **globalOffsetTable;
	void *entry;
	
	// symbol and string table of this shared object
	uintptr_t hashTableOffset;
	uintptr_t symbolTableOffset;
	uintptr_t stringTableOffset;
	
	// save the lazy JUMP_SLOT relocation table
	uintptr_t lazyRelocTableOffset;
	size_t lazyTableSize;
	bool lazyExplicitAddend;
};

void processCopyRela(SharedObject *object, Elf64_Rela *reloc);
void processCopyRelocations(SharedObject *object);

// --------------------------------------------------------
// Scope
// --------------------------------------------------------

struct Scope {
	enum : uint32_t {
		kResolveCopy = 1
	};

	Scope();

	void *resolveSymbol(const char *resolve_str,
			SharedObject *from_object, uint32_t flags);

	util::Vector<SharedObject *, Allocator> objects;
};

// --------------------------------------------------------
// Loader
// --------------------------------------------------------

class Loader {
public:
	Loader(Scope *scope);
	
	void load(SharedObject *object, const char *file);
	
	void process();

private:
	void parseDynamic(SharedObject *object);
	void processDependencies(SharedObject *object);
	void processStaticRelocations(SharedObject *object);
	void processLazyRelocations(SharedObject *object);
	
	void processRela(SharedObject *object, Elf64_Rela *reloc);

	Scope *p_scope;
	util::LinkedList<SharedObject *, Allocator> p_processQueue;
};

// --------------------------------------------------------
// Namespace scope functions
// --------------------------------------------------------

extern "C" void pltRelocateStub() __attribute__ (( visibility("hidden") ));

// --------------------------------------------------------
// Communication stuff
// --------------------------------------------------------

extern util::LazyInitializer<helx::EventHub> eventHub;
extern util::LazyInitializer<helx::Pipe> serverPipe;

