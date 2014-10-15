
#include "../../frigg/include/arch_x86/types64.hpp"
#include "runtime.hpp"
#include "debug.hpp"
#include "../../frigg/include/arch_x86/gdt.hpp"
#include "../../frigg/include/arch_x86/idt.hpp"
#include "../../frigg/include/elf.hpp"
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

void *loadInitImage(memory::PageSpace *space, char *image) {
	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)image;
	if(ehdr->e_ident[0] != '\x7F'
			|| ehdr->e_ident[1] != 'E'
			|| ehdr->e_ident[2] != 'L'
			|| ehdr->e_ident[3] != 'F') {
		vgaLogger->log("Illegal magic fields");
		debug::panic();
	}
	if(ehdr->e_type != ET_EXEC) {
		vgaLogger->log("init image must be ET_EXEC");
		debug::panic();
	}
	
	for(int i = 0; i < ehdr->e_phnum; i++) {
		Elf64_Phdr *phdr = (Elf64_Phdr*)(image + ehdr->e_phoff
				+ i * ehdr->e_phentsize);
		if(phdr->p_offset % 0x1000 != 0) {
			vgaLogger->log("PHDR not aligned in file");
			debug::panic();
		}else if(phdr->p_vaddr % 0x1000 != 0) {
			vgaLogger->log("PHDR not aligned in memory");
			debug::panic();
		}else if(phdr->p_filesz != phdr->p_memsz) {
			vgaLogger->log("PHDR file size != memory size");
			debug::panic();
		}
		
		uint32_t page = 0;
		while(page < (uint32_t)phdr->p_filesz) {
			space->mapSingle4k((void *)(phdr->p_vaddr + page),
					(uintptr_t)(image + phdr->p_offset + page));
			page += 0x1000;
		}
		vgaLogger->log("Loaded PHDR\n");
	}
	
	return (void *)ehdr->e_entry;
}

extern "C" void thorMain(uint64_t init_image) {
	vgaScreen.initialize((char *)0xB8000, 80, 25);
	
	vgaTerminal.initialize(vgaScreen.access());
	vgaTerminal->clear();

	vgaLogger.initialize(vgaTerminal.access());
	vgaLogger->log("Starting Thor");
	debug::criticalLogger = vgaLogger.access();

	stupidTableAllocator.initialize(0x400000);
	memory::tableAllocator = stupidTableAllocator.access();

	uintptr_t tss_page = stupidTableAllocator->allocate();
	
	uint32_t *tss_ptr = (uint32_t *)tss_page;
	for(int i = 0; i < 1024; i++)
		tss_ptr[i] = 0;
	tss_ptr[1] = 0x200000;

	uintptr_t gdt_page = stupidTableAllocator->allocate();
	frigg::arch_x86::makeGdtNullSegment((uint32_t *)gdt_page, 0);
	frigg::arch_x86::makeGdtCode64SystemSegment((uint32_t *)gdt_page, 1);
	frigg::arch_x86::makeGdtCode64UserSegment((uint32_t *)gdt_page, 2);
	frigg::arch_x86::makeGdtFlatData32UserSegment((uint32_t *)gdt_page, 3);
	frigg::arch_x86::makeGdtTss64Descriptor((uint32_t *)gdt_page, 4, tss_ptr);

	frigg::arch_x86::Gdtr gdtr;
	gdtr.limit = 6 * 8;
	gdtr.pointer = gdt_page;
	asm volatile ( "lgdt (%0)" : : "r"( &gdtr ) );

	thorRtLoadCs(0x8);
	
	asm volatile ( "ltr %w0" : : "r" ( 0x20 ) );

	uintptr_t idt_page = stupidTableAllocator->allocate();
	for(int i = 0; i < 256; i++)
		frigg::arch_x86::makeIdt64NullGate((uint32_t *)idt_page, i);
	frigg::arch_x86::makeIdt64IntSystemGate((uint32_t *)idt_page, 8, 0x8, (void *)&thorRtIsrDoubleFault);
	frigg::arch_x86::makeIdt64IntSystemGate((uint32_t *)idt_page, 14, 0x8, (void *)&thorRtIsrPageFault);
	frigg::arch_x86::makeIdt64IntUserGate((uint32_t *)idt_page, 0x80, 0x8, (void *)&thorRtIsrSyscall);

	frigg::arch_x86::Idtr idtr;
	idtr.limit = 16 * 256;
	idtr.pointer = idt_page;
	asm volatile ( "lidt (%0)" : : "r"( &idtr ) );

	memory::kernelSpace.initialize(0x301000);
	memory::kernelAllocator.initialize();

	void *entry = loadInitImage(memory::kernelSpace.access(), (char *)init_image);
	thorRtContinueThread(0x13, entry);
}

extern "C" void thorDoubleFault() {
	vgaLogger->log("Double fault");
	debug::panic();
}

extern "C" void thorPageFault() {
	vgaLogger->log("Page fault");
	debug::panic();
}

extern "C" void thorSyscall(uint64_t index, uint64_t arg0, uint64_t arg1,
		uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
//	vgaLogger->log("Syscall");
}

