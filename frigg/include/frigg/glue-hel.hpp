
class InfoSink {
public:
	void print(char c);
	void print(const char *str);
};

typedef frigg::debug::DefaultLogger<InfoSink> InfoLogger;
extern InfoSink infoSink;
extern frigg::LazyInitializer<InfoLogger> infoLogger;

struct VirtualAlloc {
public:
	uintptr_t map(size_t length);

	void unmap(uintptr_t address, size_t length);
};

typedef frigg::DebugAllocator<VirtualAlloc, frigg::TicketLock> Allocator;
extern VirtualAlloc virtualAlloc;
extern frigg::LazyInitializer<Allocator> allocator;

