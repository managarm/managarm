
#include "runtime.hpp"
#include "debug.hpp"
#include "util/vector.hpp"

namespace thor {

enum Error {
	kErrSuccess = 0
};

class Resource {

};

class Descriptor {

};

namespace memory {

class PageAllocator {
public:
	virtual uintptr_t allocate() = 0;
};

class StupidPageAllocator : public PageAllocator {
public:
	StupidPageAllocator(uintptr_t next_page)
			: p_nextPage(next_page) { }
	
	virtual uintptr_t allocate() {
		uintptr_t page = p_nextPage;
		p_nextPage += 0x1000;
		return page;
	}

private:
	uintptr_t p_nextPage;
};

void *physicalToVirtual(uintptr_t address) {
	return (void *)address;
}

PageAllocator *tableAllocator;

enum PageFlags {
	kPageSize = 0x1000,
	kPagePresent = 1,
	kPageWrite = 2
};

class PageSpace {
public:
	PageSpace(uintptr_t pml4_address)
			: p_pml4Address(pml4_address) { }

	void mapSingle4k(void *pointer, uintptr_t physical) {
		if((uintptr_t)pointer % 0x1000 != 0) {
			debug::criticalLogger->log("pk_page_map(): Illegal virtual address alignment");
			debug::panic();
		}
		if(physical % 0x1000 != 0) {
			debug::criticalLogger->log("pk_page_map(): Illegal physical address alignment");
			debug::panic();
		}

		int pml4_index = (int)(((uintptr_t)pointer >> 39) & 0x1FF);
		int pdpt_index = (int)(((uintptr_t)pointer >> 30) & 0x1FF);
		int pd_index = (int)(((uintptr_t)pointer >> 21) & 0x1FF);
		int pt_index = (int)(((uintptr_t)pointer >> 12) & 0x1FF);
		
		// find the pml4_entry. the pml4 exists already
		volatile uint64_t *pml4 = (uint64_t *)physicalToVirtual(p_pml4Address);
		uint64_t pml4_entry = ((uint64_t*)pml4)[pml4_index];
		
		// find the pdpt entry; create pdpt if necessary
		volatile uint64_t *pdpt = (uint64_t *)physicalToVirtual(pml4_entry & 0xFFFFF000);
		if((pml4_entry & kPagePresent) == 0) {
			uintptr_t page = tableAllocator->allocate();
			debug::criticalLogger->log("allocate pdpt");
			pdpt = (uint64_t *)physicalToVirtual(page);
			for(int i = 0; i < 512; i++)
				((uint64_t*)pdpt)[i] = 0;
			pml4[pml4_index] = page | kPagePresent | kPageWrite;
		}
		uint64_t pdpt_entry = ((uint64_t*)pdpt)[pdpt_index];
		
		// find the pd entry; create pd if necessary
		volatile uint64_t *pd = (uint64_t *)physicalToVirtual(pdpt_entry & 0xFFFFF000);
		if((pdpt_entry & kPagePresent) == 0) {
			uintptr_t page = tableAllocator->allocate();
			debug::criticalLogger->log("allocate pd");
			pd = (uint64_t *)physicalToVirtual(page);
			for(int i = 0; i < 512; i++)
				((uint64_t*)pd)[i] = 0;
			pdpt[pdpt_index] = page | kPagePresent | kPageWrite;
		}
		uint64_t pd_entry = ((uint64_t*)pd)[pd_index];
		
		// find the pt entry; create pt if necessary
		volatile uint64_t *pt = (uint64_t *)physicalToVirtual(pd_entry & 0xFFFFF000);
		if((pd_entry & kPagePresent) == 0) {
			uintptr_t page = tableAllocator->allocate();
			debug::criticalLogger->log("allocate pt");
			pt = (uint64_t *)physicalToVirtual(page);
			for(int i = 0; i < 512; i++)
				((uint64_t*)pt)[i] = 0;
			pd[pd_index] = page | kPagePresent | kPageWrite;
		}
		
		// setup the new pt entry
		if((pt[pt_index] & kPagePresent) != 0) {
			debug::criticalLogger->log("pk_page_map(): Page already mapped!");
			debug::panic();
		}
		pt[pt_index] = physical | kPagePresent | kPageWrite;
	}

private:
	uintptr_t p_pml4Address;
};

LazyInitializer<PageSpace> kernelSpace;

class StupidVirtualAllocator {
public:
	StupidVirtualAllocator() : p_nextPointer((char *)0x800000) { }

	void *allocate(size_t length) {
		if(length % kPageSize != 0)
			length += kPageSize - (length % kPageSize);

		char *pointer = p_nextPointer;
		p_nextPointer += length;
		return pointer;
	}

private:
	char *p_nextPointer;
};

class StupidMemoryAllocator {
public:
	StupidMemoryAllocator() { }

	void *allocate(size_t length) {
		void *pointer = p_virtualAllocator.allocate(length);
		for(size_t offset = 0; offset < length; offset += kPageSize)
			kernelSpace->mapSingle4k((char *)pointer + offset, tableAllocator->allocate());
		thorRtInvalidateSpace();
		return pointer;
	}

private:
	StupidVirtualAllocator p_virtualAllocator;
};

LazyInitializer<StupidMemoryAllocator> kernelAllocator;

}};

using namespace thor;

LazyInitializer<debug::VgaScreen> vgaScreen;
LazyInitializer<debug::Terminal> vgaTerminal;
LazyInitializer<debug::TerminalLogger> vgaLogger;

LazyInitializer<memory::StupidPageAllocator> stupidTableAllocator;

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

