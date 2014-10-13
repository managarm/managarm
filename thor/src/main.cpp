
#include "../../frigg/include/arch_x86/types64.hpp"
#include "runtime.hpp"
#include "debug.hpp"
#include "../../frigg/include/arch_x86/gdt.hpp"
#include "../../frigg/include/arch_x86/idt.hpp"
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

	uintptr_t gdt_page = stupidTableAllocator->allocate();
	frigg::arch_x86::makeGdtNullSegment((uint32_t *)gdt_page, 0);
	frigg::arch_x86::makeGdtCode64SystemSegment((uint32_t *)gdt_page, 1);

	frigg::arch_x86::Gdtr gdtr;
	gdtr.limit = 8 * 2;
	gdtr.pointer = gdt_page;
	asm volatile ( "lgdt (%0)" : : "r"( &gdtr ) );

	thorRtLoadCs(0x8);

	uintptr_t idt_page = stupidTableAllocator->allocate();
	for(int i = 0; i < 256; i++)
		frigg::arch_x86::makeIdt64NullGate((uint32_t *)idt_page, i);
	frigg::arch_x86::makeIdt64IntGate((uint32_t *)idt_page, 8, 0x8, (void *)&thorRtIsrDoubleFault);
	frigg::arch_x86::makeIdt64IntGate((uint32_t *)idt_page, 14, 0x8, (void *)&thorRtIsrPageFault);

	frigg::arch_x86::Idtr idtr;
	idtr.limit = 16 * 256;
	idtr.pointer = idt_page;
	asm volatile ( "lidt (%0)" : : "r"( &idtr ) );

	memory::kernelSpace.initialize(0x301000);
	memory::kernelAllocator.initialize();

	util::Vector<int, memory::StupidMemoryAllocator> vector(memory::kernelAllocator.access());
	vector.push(42);
	vector.push(21);
	vector.push(99);

	for(int i = 0; i < vector.size(); i++)
		vgaLogger->log(vector[i]);
}

extern "C" void thorDoubleFault() {
	vgaLogger->log("Double fault");
	debug::panic();
}

extern "C" void thorPageFault() {
	vgaLogger->log("Page fault");
	debug::panic();
}

