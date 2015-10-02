
#ifndef FRIGG_GLUE_HEL_HPP
#define FRIGG_GLUE_HEL_HPP

#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>

class InfoSink {
public:
	void print(char c);
	void print(const char *str);
};

typedef frigg::DefaultLogger<InfoSink> InfoLogger;
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

#endif // FRIGG_GLUE_HEL_HPP

