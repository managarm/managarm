
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/initializer.hpp>
#include <frigg/support.hpp>
#include <frigg/debug.hpp>
#include <frigg/libc.hpp>
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
frigg::util::LazyInitializer<InfoLogger> infoLogger;

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
	ASSERT((length % 0x1000) == 0);

	HelHandle memory;
	void *actual_ptr;
	helAllocateMemory(length, &memory);
	helMapMemory(memory, kHelNullHandle, nullptr, length,
			kHelMapReadWrite, &actual_ptr);
	return (uintptr_t)actual_ptr;
}

void VirtualAlloc::unmap(uintptr_t address, size_t length) {

}

VirtualAlloc virtualAlloc;
frigg::util::LazyInitializer<Allocator> allocator;

