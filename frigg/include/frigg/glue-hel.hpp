
class InfoSink {
public:
	void print(char c);
	void print(const char *str);
};

typedef frigg::debug::DefaultLogger<InfoSink> InfoLogger;
extern InfoSink infoSink;
extern frigg::util::LazyInitializer<InfoLogger> infoLogger;

struct VirtualAlloc {
public:
	uintptr_t map(size_t length);

	void unmap(uintptr_t address, size_t length);
};

typedef frigg::memory::DebugAllocator<VirtualAlloc> Allocator;
extern VirtualAlloc virtualAlloc;
extern frigg::util::LazyInitializer<Allocator> allocator;

