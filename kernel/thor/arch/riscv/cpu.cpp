#include <frg/manual_box.hpp>
#include <generic/thor-internal/cpu-data.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/unimplemented.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/main.hpp>

namespace thor {

void enableUserAccess() { unimplementedOnRiscv(); }
void disableUserAccess() { unimplementedOnRiscv(); }

void UserContext::deactivate() { unimplementedOnRiscv(); }

smarter::borrowed_ptr<Thread> activeExecutor() { unimplementedOnRiscv(); }

Error getEntropyFromCpu(void *buffer, size_t size) { unimplementedOnRiscv(); }

void doRunOnStack(void (*function)(void *, void *), void *sp, void *argument) {
	unimplementedOnRiscv();
}

} // namespace thor
