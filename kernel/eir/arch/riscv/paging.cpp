#include <eir-internal/arch.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/arch/riscv/paging.hpp>

namespace eir {

sv39_page_table_entry* pt2 = nullptr;

void mapSingle4kPage(address_t address, address_t physical, uint32_t flags,
		CachingMode caching_mode) {
	uint32_t vpn0 = getPPN0FromAddress(address);
	uint32_t vpn1 = getPPN1FromAddress(address);
	uint32_t vpn2 = getVPN2FromAddress(address);

	// Check whether the subtable exists for PT1.
	if(!pt2[vpn2].valid) {
		// Create a new table.
		uintptr_t subtable = allocPage();
		pt2[vpn2].packPhyiscalAddress(subtable);
		pt2[vpn2].valid = true;
	}

	sv39_page_table_entry* pt1 = pt2[vpn2].getSubtable();
	// Check whether the subtable exists for PT2.
	if(!pt1[vpn1].valid) {
		// Create a new table.
		uintptr_t subtable = allocPage();
		pt1[vpn1].packPhyiscalAddress(subtable);
		pt1[vpn1].valid = true;
	}

	sv39_page_table_entry* pt0 = pt1[vpn1].getSubtable();
	if(pt0[vpn0].valid) {
		eir::panicLogger() << "eir: tried to map the same page twice!" << frg::endlog;
	}

	sv39_page_table_entry* page = pt1 + vpn1;
	page->packPhyiscalAddress(physical);
	page->valid = true;
	page->write = flags & PageFlags::write;
	page->execute = flags & PageFlags::execute;
	page->read = true;
	page->global = flags & PageFlags::global;

	(void)caching_mode;
}

#ifndef EIR_EFI

void initProcessorPaging(void *kernel_start, uint64_t &kernel_entry) {
	// TODO
	eir::panicLogger() << "eir: TODO: implement paging on non-EFI devices" << frg::endlog;
	(void)kernel_start;
	(void)kernel_entry;
}

#endif

} // namespace eir
