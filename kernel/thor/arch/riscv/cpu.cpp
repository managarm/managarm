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

bool iseqStore64(uint64_t *p, uint64_t v) {
	// TODO: This is a shim. A proper implementation is needed for NMIs on ARM.
	std::atomic_ref{*p}.store(v, std::memory_order_relaxed);
	return true;
}

bool iseqCopyWeak(void *dst, const void *src, size_t size) {
	// TODO: This is a shim. A proper implementation is needed for NMIs on ARM.
	memcpy(dst, src, size);
	return true;
}

void UserContext::deactivate() { unimplementedOnRiscv(); }

void saveExecutor(Executor *executor, FaultImageAccessor accessor) { unimplementedOnRiscv(); }
void saveExecutor(Executor *executor, IrqImageAccessor accessor) { unimplementedOnRiscv(); }
void saveExecutor(Executor *executor, SyscallImageAccessor accessor) { unimplementedOnRiscv(); }
void workOnExecutor(Executor *executor) { unimplementedOnRiscv(); }

void scrubStack(FaultImageAccessor accessor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);
	;
}

void scrubStack(IrqImageAccessor accessor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);
	;
}

void scrubStack(SyscallImageAccessor accessor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(accessor.frameBase()), cont);
	;
}

void scrubStack(Executor *executor, Continuation cont) {
	scrubStackFrom(reinterpret_cast<uintptr_t>(*executor->sp()), cont);
}

smarter::borrowed_ptr<Thread> activeExecutor() { unimplementedOnRiscv(); }

Error getEntropyFromCpu(void *buffer, size_t size) { return Error::noHardwareSupport; }

void doRunOnStack(void (*function)(void *, void *), void *sp, void *argument) {
	unimplementedOnRiscv();
}

} // namespace thor
