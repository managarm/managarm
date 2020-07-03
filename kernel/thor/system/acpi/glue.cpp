#include <limits.h>

#include <frigg/debug.hpp>
#include <frigg/printf.hpp>
#include <frigg/memory.hpp>
#include "../../arch/x86/paging.hpp"
#include "../../arch/x86/pic.hpp"
#include <thor-internal/irq.hpp>
#include <thor-internal/kernel_heap.hpp>
#include "../../system/pci/pci.hpp"

#include <acpispec/tables.h>
#include <lai/host.h>

namespace thor {
namespace acpi {
	extern void *globalRsdtWindow;
	extern int globalRsdtVersion;
} }

using namespace thor;

void laihost_log(int, const char *msg) {
	frigg::infoLogger() << "lai: " << msg << frigg::endLog;
}

void laihost_panic(const char *msg) {
	frigg::panicLogger() << "\e[31m" "lai panic: " << msg << "\e[39m" << frigg::endLog;
	__builtin_unreachable();
}

void *laihost_malloc(size_t size) {
	return kernelAlloc->allocate(size);
}

void *laihost_realloc(void *ptr, size_t size) {
	return kernelAlloc->reallocate(ptr, size);
}

void laihost_free(void *ptr) {
	kernelAlloc->free(ptr);
}

// TODO: We do not want to keep things mapped forever.
void *laihost_map(size_t physical, size_t length) {
	auto pow2ceil = [] (unsigned long s) {
		assert(s);
		return 1 << (sizeof(unsigned long) * CHAR_BIT - __builtin_clzl(s - 1));
	};

	auto paddr = physical & ~(kPageSize - 1);
	auto vsize = length + (physical & (kPageSize - 1));
	size_t msize = pow2ceil(frg::max(vsize, static_cast<size_t>(0x10000)));

	auto ptr = KernelVirtualMemory::global().allocate(msize);
	for(size_t pg = 0; pg < vsize; pg += kPageSize)
		KernelPageSpace::global().mapSingle4k((VirtualAddr)ptr + pg, paddr + pg,
				page_access::write, CachingMode::null);
	return reinterpret_cast<char *>(ptr) + (physical & (kPageSize - 1));
}

void laihost_unmap(void *ptr, size_t length) {
	auto pow2ceil = [] (unsigned long s) {
		assert(s);
		return 1 << (sizeof(unsigned long) * CHAR_BIT - __builtin_clzl(s - 1));
	};

	auto vaddr = reinterpret_cast<uintptr_t>(ptr) & ~(kPageSize - 1);
	auto vsize = length + (reinterpret_cast<uintptr_t>(ptr) & (kPageSize - 1));
	size_t msize = pow2ceil(frg::max(vsize, static_cast<size_t>(0x10000)));

	for(size_t pg = 0; pg < vsize; pg += kPageSize)
		KernelPageSpace::global().unmapSingle4k(vaddr + pg);
	// TODO: free the virtual memory range.
	(void)msize;
}

static void *mapTable(uintptr_t address) {
	auto headerWindow = laihost_map(address, sizeof(acpi_header_t));
	auto headerPtr = reinterpret_cast<acpi_header_t *>(headerWindow);
	return laihost_map(address, headerPtr->length);
}

static void *scanRsdt(const char *name, size_t index) {
	if(thor::acpi::globalRsdtVersion == 1){
		auto rsdt = reinterpret_cast<acpi_rsdt_t *>(thor::acpi::globalRsdtWindow);
		assert(rsdt->header.length >= sizeof(acpi_header_t));

		size_t n = 0;
		int numPtrs = (rsdt->header.length - sizeof(acpi_header_t)) / sizeof(uint32_t);
		for(int i = 0; i < numPtrs; i++) {
			auto tableWindow = reinterpret_cast<acpi_header_t *>(mapTable(rsdt->tables[i]));
			char sig[5];
			sig[4] = 0;
			memcpy(sig, tableWindow->signature, 4);
			if(memcmp(tableWindow->signature, name, 4))
				continue;
			if(n == index)
				return tableWindow;
			n++;
		}
	} else if(thor::acpi::globalRsdtVersion == 2){
		auto xsdt = reinterpret_cast<acpi_xsdt_t *>(thor::acpi::globalRsdtWindow);
		assert(xsdt->header.length >= sizeof(acpi_header_t));

		size_t n = 0;
		int numPtrs = (xsdt->header.length - sizeof(acpi_header_t)) / sizeof(uint64_t);
		for(int i = 0; i < numPtrs; i++) {
			auto tableWindow = reinterpret_cast<acpi_header_t *>(mapTable(xsdt->tables[i]));
			char sig[5];
			sig[4] = 0;
			memcpy(sig, tableWindow->signature, 4);
			if(memcmp(tableWindow->signature, name, 4))
				continue;
			if(n == index)
				return tableWindow;
			n++;
		}
	} else {
		assert(!"Unknown acpi version in scanRsdt");
	}

	return nullptr;
}

void *laihost_scan(const char *name, size_t index) {
	if(!memcmp(name, "DSDT", 4)) {
		void *fadtWindow = scanRsdt("FACP", 0);
		assert(fadtWindow);
		auto fadt = reinterpret_cast<acpi_fadt_t *>(fadtWindow);
		void *dsdtWindow = mapTable(fadt->dsdt);
		return dsdtWindow;
	}else{
		return scanRsdt(name, index);
	}
}

void laihost_outb(uint16_t p, uint8_t v) {
	asm volatile ("outb %0, %1" : : "a"(v), "d"(p));
}
void laihost_outw(uint16_t p, uint16_t v) {
	asm volatile ("outw %0, %1" : : "a"(v), "d"(p));
}
void laihost_outd(uint16_t p, uint32_t v) {
	asm volatile ("outl %0, %1" : : "a"(v), "d"(p));
}

uint8_t laihost_inb(uint16_t p) {
	uint8_t v;
	asm volatile ("inb %1, %0" : "=a"(v) : "d"(p));
	return v;
}
uint16_t laihost_inw(uint16_t p) {
	uint16_t v;
	asm volatile ("inw %1, %0" : "=a"(v) : "d"(p));
	return v;
}
uint32_t laihost_ind(uint16_t p) {
	uint32_t v;
	asm volatile ("inl %1, %0" : "=a"(v) : "d"(p));
	return v;
}

void laihost_pci_writeb(uint16_t, uint8_t bus, uint8_t slot, uint8_t fn,
		uint16_t offset, uint8_t v) {
	writePciByte(bus, slot, fn, offset, v);
}
void laihost_pci_writew(uint16_t, uint8_t bus, uint8_t slot, uint8_t fn,
		uint16_t offset, uint16_t v) {
	writePciHalf(bus, slot, fn, offset, v);
}
void laihost_pci_writed(uint16_t, uint8_t bus, uint8_t slot, uint8_t fn,
		uint16_t offset, uint32_t v) {
	writePciWord(bus, slot, fn, offset, v);
}

uint8_t laihost_pci_readb(uint16_t, uint8_t bus, uint8_t slot, uint8_t fn, uint16_t offset) {
	return readPciByte(bus, slot, fn, offset);
}
uint16_t laihost_pci_readw(uint16_t, uint8_t bus, uint8_t slot, uint8_t fn, uint16_t offset) {
	return readPciHalf(bus, slot, fn, offset);
}
uint32_t laihost_pci_readd(uint16_t, uint8_t bus, uint8_t slot, uint8_t fn, uint16_t offset) {
	return readPciWord(bus, slot, fn, offset);
}

void laihost_sleep(uint64_t) { }
