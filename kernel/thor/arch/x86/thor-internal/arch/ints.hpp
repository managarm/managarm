#pragma once

#include <atomic>

#include <frigg/atomic.hpp>
#include <frigg/debug.hpp>

namespace thor {

void setupEarlyInterruptHandlers();

void setupIdt(uint32_t *table);

inline bool intsAreEnabled() {
	uint64_t rflags;
	asm volatile (
		"pushfq\n"
		"\rpop %0"
		: "=r" (rflags)
	);
	return (rflags & 0x200) != 0;
}

inline void enableInts() {
	asm volatile ("sti");
}

inline void disableInts() {
	asm volatile ("cli");
}

inline void halt() {
	asm volatile ("hlt");
}

void suspendSelf();

void sendPingIpi(int id);

} // namespace thor
