
#ifndef FRIGG_GLUE_HEL_HPP
#define FRIGG_GLUE_HEL_HPP

#include <frigg/cxx-support.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>
#include <frigg/memory.hpp>

class InfoSink {
public:
	void print(char c);
	void print(const char *str);
};

extern InfoSink infoSink;

struct VirtualAlloc {
public:
	uintptr_t map(size_t length);

	void unmap(uintptr_t address, size_t length);
};

typedef frigg::SlabAllocator<VirtualAlloc, frigg::TicketLock> Allocator;
extern VirtualAlloc virtualAlloc;
extern frigg::LazyInitializer<Allocator> allocator;

#endif // FRIGG_GLUE_HEL_HPP

