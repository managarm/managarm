#include <thor-internal/arch/cpu.hpp>
#include <generic/thor-internal/cpu-data.hpp>
#include <frg/manual_box.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/fiber.hpp>

namespace thor {

void enableUserAccess() { }
void disableUserAccess() { }

void UserContext::deactivate() { }

smarter::borrowed_ptr<Thread> activeExecutor() { assert(!"Not implemented"); }

Error getEntropyFromCpu(void *buffer, size_t size) { assert(!"Not implemented"); }

size_t getCpuCount() { assert(!"Not implemented"); }

}
