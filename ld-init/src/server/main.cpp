
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/initializer.hpp>
#include <frigg/support.hpp>
#include <frigg/debug.hpp>
#include <frigg/memory.hpp>
#include <frigg/libc.hpp>
#include <frigg/elf.hpp>

#include <hel.h>
#include <hel-syscalls.h>

#include <frigg/glue-hel.hpp>

namespace debug = frigg::debug;

int main() {
	infoLogger.initialize(infoSink);
	infoLogger->log() << "Entering ld-server" << debug::Finish();
	allocator.initialize(virtualAlloc);
}

