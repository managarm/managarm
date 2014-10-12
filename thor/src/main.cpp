
#include "../../frigg/include/arch_x86/types64.hpp"
#include "runtime.hpp"
#include "debug.hpp"
#include "util/vector.hpp"
#include "memory/physical-alloc.hpp"
#include "memory/paging.hpp"
#include "memory/kernel-alloc.hpp"

namespace thor {

enum Error {
	kErrSuccess = 0
};

class Resource {

};

class Descriptor {

};

}

using namespace thor;

LazyInitializer<debug::VgaScreen> vgaScreen;
LazyInitializer<debug::Terminal> vgaTerminal;
LazyInitializer<debug::TerminalLogger> vgaLogger;

LazyInitializer<memory::StupidPhysicalAllocator> stupidTableAllocator;

extern "C" void thorMain() {
	vgaScreen.initialize((char *)0xB8000, 80, 25);
	
	vgaTerminal.initialize(vgaScreen.access());
	vgaTerminal->clear();

	vgaLogger.initialize(vgaTerminal.access());
	vgaLogger->log("Starting Thor");
	debug::criticalLogger = vgaLogger.access();

	stupidTableAllocator.initialize(0x400000);
	memory::tableAllocator = stupidTableAllocator.access();

	memory::kernelSpace.initialize(0x301000);
	memory::kernelAllocator.initialize();

	util::Vector<int, memory::StupidMemoryAllocator> vector(memory::kernelAllocator.access());
	vector.push(42);
	vector.push(21);
	vector.push(99);

	for(int i = 0; i < vector.size(); i++)
		vgaLogger->log(vector[i]);
}

