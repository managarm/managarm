
#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/glue-hel.hpp>

#include <hel.h>
#include <hel-syscalls.h>

void InfoSink::print(char c) {
	helLog(&c, 1);
}

void InfoSink::print(const char *str) {
	size_t length = 0;
	for(size_t i = 0; str[i] != 0; i++)
		length++;
	helLog(str, length);
}

InfoSink infoSink;

void friggPrintCritical(char c) {
	infoSink.print(c);
}

void friggPrintCritical(const char *str) {
	infoSink.print(str);
}

void friggPanic() {
	helPanic("Abort", 5);
	__builtin_unreachable();
}

uintptr_t VirtualAlloc::map(size_t length) {
	assert((length % 0x1000) == 0);

	HelHandle memory;
	void *actual_ptr;
	HEL_CHECK(helAllocateMemory(length, 0, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr, 0, length,
			kHelMapReadWrite, &actual_ptr));
	HEL_CHECK(helCloseDescriptor(memory));
	return (uintptr_t)actual_ptr;
}

void VirtualAlloc::unmap(uintptr_t address, size_t length) {
	HEL_CHECK(helUnmapMemory(kHelNullHandle, (void *)address, length));
}

VirtualAlloc virtualAlloc;
frigg::LazyInitializer<Allocator> allocator;

