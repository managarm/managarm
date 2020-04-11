
#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>
#include <frigg/array.hpp>
#include <frigg/elf.hpp>
#include <frigg/arch_x86/machine.hpp>
#include <frigg/arch_x86/gdt.hpp>
#include <frigg/libc.hpp>
#include <frigg/string.hpp>
#include <frigg/support.hpp>
#include <render-text.hpp>
#include <physical-buddy.hpp>
#include "main.hpp"

namespace arch = frigg::arch_x86;

address_t bootMemoryLimit;
address_t allocatedMemory;

// ----------------------------------------------------------------------------
// Memory region management.
// ----------------------------------------------------------------------------

Region regions[numRegions];

Region *obtainRegion() {
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::null)
			continue;
		regions[i].regionType = RegionType::unconstructed;
		return &regions[i];
	}
	frigg::panicLogger() << "Eir: Memory region limit exhausted" << frigg::endLog;
}

void createInitialRegion(address_t base, address_t size) {
	auto limit = base + size;

	// For now we do not touch memory that is required during boot.
	address_t address = frigg::max(base, bootMemoryLimit);
	
	// Align address to 2 MiB.
	// This ensures thor can allocate contiguous chunks of up to 2 MiB.
	address = (address + 0x1FFFFF) & ~address_t(0x1FFFFF);

	if(address >= limit) {
		frigg::infoLogger() << "eir: Discarding memory region at 0x"
				<< frigg::logHex(base) << " (smaller than alignment)" << frigg::endLog;
		return;
	}
	
	// Trash the initial memory to find bugs in thor.
	// TODO: This code fails for size > 2^32.
	/*
	auto accessor = reinterpret_cast<uint8_t *>(address);
	uint64_t pattern = 0xB306'94E7'F8D2'78AB;
	for(ptrdiff_t i = 0; i < limit - address; ++i) {
		accessor[i] = pattern;
		pattern = (pattern << 8) | (pattern >> 56);
	}
	asm volatile ("" : : : "memory");
	*/

	// For now we ensure that the kernel has some memory to work with.
	// TODO: Handle small memory regions.
	if(limit - address < 32 * address_t(0x100000)) {
		frigg::infoLogger() << "eir: Discarding memory region at 0x"
				<< frigg::logHex(base) << " (smaller than minimum size)" << frigg::endLog;
		return;
	}

	assert(!(address % kPageSize));
	assert(!(limit % kPageSize));

	auto region = obtainRegion();
	region->regionType = RegionType::allocatable;
	region->address = address;
	region->size = limit - address;
}

address_t cutFromRegion(size_t size) {
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::allocatable)
			continue;

		if(regions[i].size < size)
			continue;

		regions[i].size -= size;
		return regions[i].address + regions[i].size;
	}
	
	frigg::panicLogger() << "Eir: Unable to cut memory from a region" << frigg::endLog;
}

void setupRegionStructs() {
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::allocatable)
			continue;

		// Setup a buddy allocator.
		auto order = BuddyAccessor::suitableOrder(regions[i].size >> kPageShift);
		auto pre_roots = regions[i].size >> (kPageShift + order);
		auto overhead = BuddyAccessor::determineSize(pre_roots, order);
		overhead = (overhead + address_t(kPageSize - 1)) & ~address_t(kPageSize - 1);

		// Note that cutFromRegion might actually reduce this regions' size.
		auto table_paddr = cutFromRegion(overhead);
		auto num_roots = regions[i].size >> (kPageShift + order);
		assert(num_roots >= 32);

		regions[i].order = order;
		regions[i].numRoots = num_roots;
		regions[i].buddyTree = table_paddr;
		regions[i].buddyOverhead = overhead;

		// Finally initialize the buddy tree.
		auto table_ptr = reinterpret_cast<int8_t *>(table_paddr);
		BuddyAccessor::initialize(table_ptr, num_roots, order);

		// Setup the struct Page area.
		// TODO: It is not clear that we want page structs. Remove this?
		//auto num_pages = regions[i].size >> kPageShift;
		//auto structs_size = (num_pages * 32 + uint64_t(kPageSize - 1))
		//		& ~(uint64_t(kPageSize - 1));
		//regions[i].pageStructs = cutFromRegion(structs_size);
	}
}

// ----------------------------------------------------------------------------

constexpr int fontWidth = 8;
constexpr int fontHeight = 16;

void *displayFb;
int displayWidth;
int displayHeight;
int displayPitch;
int outputX;
int outputY;

class BochsSink {
public:
	void print(char c);
	void print(const char *str);
};

void BochsSink::print(char c) {
	auto display = [] (char c) {
		renderChars(displayFb, displayPitch / sizeof(uint32_t),
				outputX, outputY, &c, 1, 15, -1,
				std::integral_constant<int, fontWidth>{},
				std::integral_constant<int, fontHeight>{});
	};

	if(displayFb) {
		if(c == '\n') {
			outputX = 0;
			outputY++;
		}else if(outputX >= displayWidth / fontWidth) {
			outputX = 0;
			outputY++;
		}else if(outputY >= displayHeight / fontHeight) {
			// TODO: Scroll.
		}else{
			display(c);
			outputX++;
		}
	}

	arch::ioOutByte(0xE9, c);
}
void BochsSink::print(const char *str) {
	while(*str)
		print(*(str++));
}

BochsSink infoSink;

void friggBeginLog() { }
void friggEndLog() { }

void friggPrintCritical(char c) {
	infoSink.print(c);
}

void friggPrintCritical(const char *str) {
	infoSink.print(str);
}

void friggPanic() {
	while(true) { }
	__builtin_unreachable();
}

enum PageFlags {
	kPagePresent = 1,
	kPageWrite = 2,
	kPageUser = 4,
	kPagePwt = 0x8,
	kPagePat = 0x80,
	kPageGlobal = 0x100,
	kPageXd = 0x8000000000000000
};

uintptr_t bootReserve(size_t length, size_t alignment) {
	assert(length <= kPageSize);
	assert(alignment <= kPageSize);

	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::allocatable)
			continue;

		auto table = reinterpret_cast<int8_t *>(regions[i].buddyTree);
		BuddyAccessor accessor{regions[i].address, kPageShift,
				table, regions[i].numRoots, regions[i].order};
		auto physical = accessor.allocate(0, 32);
		if(physical == BuddyAccessor::illegalAddress)
			continue;
		return physical;
	}

	frigg::panicLogger() << "Eir: Out of memory" << frigg::endLog;
}



uintptr_t allocPage() {
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::allocatable)
			continue;

		auto table = reinterpret_cast<int8_t *>(regions[i].buddyTree);
		BuddyAccessor accessor{regions[i].address, kPageShift,
				table, regions[i].numRoots, regions[i].order};
		auto physical = accessor.allocate(0, 32);
		if(physical == BuddyAccessor::illegalAddress)
			continue;
		allocatedMemory += kPageSize;
	//		frigg::infoLogger() << "Allocate " << (void *)physical << frigg::endLog;
		return physical;
	}

	frigg::panicLogger() << "Eir: Out of memory" << frigg::endLog;
}

uintptr_t eirPml4Pointer = 0;

void setupPaging() {
	eirPml4Pointer = allocPage();
	for(int i = 0; i < 512; i++)
		((uint64_t*)eirPml4Pointer)[i] = 0;
	
	for(int i = 256; i < 512; i++) {
		uintptr_t pdpt_page = allocPage();
		uint64_t *pdpt_pointer = (uint64_t *)pdpt_page;
		for(int j = 0; j < 512; j++)
			pdpt_pointer[j] = 0;

		((uint64_t*)eirPml4Pointer)[i] = pdpt_page | kPagePresent | kPageWrite;
	}
}

// generates a page table where the first entry points to the table itself.
uint64_t allocPt() {
	uint64_t address = allocPage();

	auto entries = reinterpret_cast<uint64_t *>(address);
	for(int i = 0; i < 512; i++)
		entries[i] = 0;
	return address;
}

void mapPt(uint64_t address, uint64_t pt) {
	assert(address % 0x1000 == 0);

	int pml4_index = (int)((address >> 39) & 0x1FF);
	int pdpt_index = (int)((address >> 30) & 0x1FF);
	int pd_index = (int)((address >> 21) & 0x1FF);
	int pt_index = (int)((address >> 12) & 0x1FF);
	assert(!pt_index);
	
	// find the pml4_entry. the pml4 is always present
	uintptr_t pml4 = eirPml4Pointer;
	uint64_t pml4_entry = ((uint64_t*)pml4)[pml4_index];
	
	// find the pdpt entry; create pdpt if necessary
	uintptr_t pdpt = (uintptr_t)(pml4_entry & 0xFFFFF000);
	if((pml4_entry & kPagePresent) == 0) {
		pdpt = allocPage();
		for(int i = 0; i < 512; i++)
			((uint64_t*)pdpt)[i] = 0;
		((uint64_t*)pml4)[pml4_index] = pdpt | kPagePresent | kPageWrite;
	}
	uint64_t pdpt_entry = ((uint64_t*)pdpt)[pdpt_index];
	
	// find the pd entry; create pd if necessary
	uintptr_t pd = (uintptr_t)(pdpt_entry & 0xFFFFF000);
	if((pdpt_entry & kPagePresent) == 0) {
		pd = allocPage();
		for(int i = 0; i < 512; i++)
			((uint64_t*)pd)[i] = 0;
		((uint64_t*)pdpt)[pdpt_index] = pd | kPagePresent | kPageWrite;
	}
	assert(!(((uint64_t *)pd)[pd_index] & kPagePresent));
	((uint64_t *)pd)[pd_index] = pt | kPagePresent | kPageWrite;
}

void mapSingle4kPage(uint64_t address, uint64_t physical, uint32_t flags,
		CachingMode caching_mode = CachingMode::null) {
	assert(address % 0x1000 == 0);
	assert(physical % 0x1000 == 0);

	int pml4_index = (int)((address >> 39) & 0x1FF);
	int pdpt_index = (int)((address >> 30) & 0x1FF);
	int pd_index = (int)((address >> 21) & 0x1FF);
	int pt_index = (int)((address >> 12) & 0x1FF);
	
	// find the pml4_entry. the pml4 is always present
	uintptr_t pml4 = eirPml4Pointer;
	uint64_t pml4_entry = ((uint64_t*)pml4)[pml4_index];
	
	// find the pdpt entry; create pdpt if necessary
	uintptr_t pdpt = (uintptr_t)(pml4_entry & 0xFFFFF000);
	if(!(pml4_entry & kPagePresent)) {
		pdpt = allocPage();
		for(int i = 0; i < 512; i++)
			((uint64_t*)pdpt)[i] = 0;
		((uint64_t*)pml4)[pml4_index] = pdpt | kPagePresent | kPageWrite;
	}
	uint64_t pdpt_entry = ((uint64_t*)pdpt)[pdpt_index];
	
	// find the pd entry; create pd if necessary
	uintptr_t pd = (uintptr_t)(pdpt_entry & 0xFFFFF000);
	if(!(pdpt_entry & kPagePresent)) {
		pd = allocPage();
		for(int i = 0; i < 512; i++)
			((uint64_t*)pd)[i] = 0;
		((uint64_t*)pdpt)[pdpt_index] = pd | kPagePresent | kPageWrite;
	}
	uint64_t pd_entry = ((uint64_t*)pd)[pd_index];
	
	// find the pt entry; create pt if necessary
	uintptr_t pt = (uintptr_t)(pd_entry & 0xFFFFF000);
	if(!(pd_entry & kPagePresent)) {
		pt = allocPage();
		for(int i = 0; i < 512; i++)
			((uint64_t*)pt)[i] = 0;
		((uint64_t*)pd)[pd_index] = pt | kPagePresent | kPageWrite;
	}
	uint64_t pt_entry = ((uint64_t*)pt)[pt_index];
	
	// setup the new pt entry
	if(pt_entry & kPagePresent)
		frigg::infoLogger() << "eir: Trying to map 0x" << frigg::logHex(address)
				<< " twice!" << frigg::endLog;
	assert(!(pt_entry & kPagePresent));
	uint64_t new_entry = physical | kPagePresent;
	if(flags & kAccessWrite)
		new_entry |= kPageWrite;
	if(!(flags & kAccessExecute))
		new_entry |= kPageXd;
	if(!(flags & kAccessGlobal))
		new_entry |= kPageGlobal;
	if(caching_mode == CachingMode::writeCombine) {
		new_entry |= kPagePat | kPagePwt;
	}else{
		assert(caching_mode == CachingMode::null);
	}
	((uint64_t*)pt)[pt_index] = new_entry;
}

// ----------------------------------------------------------------------------

void mapRegionsAndStructs() {
	// This region should be available RAM on every PC.
	for(size_t page = 0x8000; page < 0x80000; page += kPageSize)
			mapSingle4kPage(0xFFFF'8000'0000'0000 + page,
					page, kAccessWrite | kAccessGlobal);

	address_t tree_mapping = 0xFFFF'C080'0000'0000;
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::allocatable)
			continue;

		// Map the region itself.
		for(address_t page = 0; page < regions[i].size; page += kPageSize)
			mapSingle4kPage(0xFFFF'8000'0000'0000 + regions[i].address + page,
					regions[i].address + page, kAccessWrite | kAccessGlobal);

		// Map the buddy tree.
		regions[i].buddyMap = tree_mapping;

		auto overhead = BuddyAccessor::determineSize(regions[i].numRoots, regions[i].order);
		for(address_t page = 0; page < overhead; page += kPageSize) {
			mapSingle4kPage(tree_mapping, regions[i].buddyTree + page, kAccessWrite | kAccessGlobal);
			tree_mapping += kPageSize;
		}
	}
}

// ----------------------------------------------------------------------------
// Bootstrap information handling.
// ----------------------------------------------------------------------------

address_t bootstrapDataPointer = 0x40000000;

address_t mapBootstrapData(void *p) {
	auto pointer = bootstrapDataPointer;
	bootstrapDataPointer += kPageSize;
	mapSingle4kPage(pointer, (address_t)p, 0);
	return pointer;
}

// ----------------------------------------------------------------------------

// note: we are loading the segments to their p_paddr addresses
// instead of the usual p_vaddr addresses!
void loadKernelImage(void *image, uint64_t *out_entry) {
	Elf64_Ehdr *ehdr = (Elf64_Ehdr *)image;
	if(ehdr->e_ident[0] != '\x7F'
			|| ehdr->e_ident[1] != 'E'
			|| ehdr->e_ident[2] != 'L'
			|| ehdr->e_ident[3] != 'F') {
		frigg::panicLogger() << "Illegal magic fields" << frigg::endLog;
	}
	assert(ehdr->e_type == ET_EXEC);
	
	for(int i = 0; i < ehdr->e_phnum; i++) {
		Elf64_Phdr *phdr = (Elf64_Phdr *)((uintptr_t)image
				+ (uintptr_t)ehdr->e_phoff
				+ i * ehdr->e_phentsize);
		assert((phdr->p_offset % 0x1000) == 0);
		assert((phdr->p_paddr % 0x1000) == 0);
		
		if(phdr->p_type != PT_LOAD)
			continue;

		uint32_t map_flags = kAccessGlobal;
		if((phdr->p_flags & (PF_R | PF_W | PF_X)) == PF_R) {
			// no additional flags
		}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
			map_flags |= kAccessWrite;
		}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
			map_flags |= kAccessExecute;
		}else{
			frigg::panicLogger() << "Illegal combination of segment permissions"
					<< frigg::endLog;
		}

		uintptr_t pg = 0;
		while(pg < (uintptr_t)phdr->p_memsz) {
			auto backing = allocPage();
			memset(reinterpret_cast<void *>(backing), 0, kPageSize);
			if(pg < (uintptr_t)phdr->p_filesz)
				memcpy(reinterpret_cast<void *>(backing),
						reinterpret_cast<void *>((uintptr_t)image + (uintptr_t)phdr->p_offset + pg),
						frigg::min(kPageSize, (uintptr_t)phdr->p_filesz - pg));
			mapSingle4kPage(phdr->p_paddr + pg, backing, map_flags);
			pg += kPageSize;
		}
	}
	
	*out_entry = ehdr->e_entry;
}

void initProcessorEarly(){
	frigg::infoLogger() << "Starting Eir" << frigg::endLog;

	frigg::Array<uint32_t, 4> vendor_res = arch::cpuid(0);
	char vendor_str[13];
	memcpy(&vendor_str[0], &vendor_res[1], 4);
	memcpy(&vendor_str[4], &vendor_res[3], 4);
	memcpy(&vendor_str[8], &vendor_res[2], 4);
	vendor_str[12] = 0;
	frigg::infoLogger() << "CPU vendor: " << (const char *)vendor_str << frigg::endLog;
	
	// Make sure everything we require is supported by the CPU.
	frigg::Array<uint32_t, 4> extended = arch::cpuid(arch::kCpuIndexExtendedFeatures);
	if((extended[3] & arch::kCpuFlagLongMode) == 0)
		frigg::panicLogger() << "Long mode is not supported on this CPU" << frigg::endLog;
	if((extended[3] & arch::kCpuFlagNx) == 0)
		frigg::panicLogger() << "NX bit is not supported on this CPU" << frigg::endLog;

	frigg::Array<uint32_t, 4> normal = arch::cpuid(arch::kCpuIndexFeatures);
	if((normal[3] & arch::kCpuFlagPat) == 0)
		frigg::panicLogger() << "PAT is not supported on this CPU" << frigg::endLog;

	initArchCpu();

	// Program the PAT. Each byte configures a single entry.
	// 00: Uncacheable
	// 01: Write Combining
	// 04: Write Through
	// 06: Write Back
	// Keep in sync with the SMP trampoline in thor.
	uint64_t pat = 0x00'00'01'00'00'00'04'06;
	frigg::arch_x86::wrmsr(0x277, pat);
}

// Returns Core region index
void initProcessorPaging(void *kernel_start, uint64_t& kernel_entry){
	setupPaging();
	frigg::infoLogger() << "eir: Allocated " << (allocatedMemory >> 10) << " KiB"
			" after setting up paging" << frigg::endLog;

	// Identically map the first 128 MiB so that we can activate paging
	// without causing a page fault.
	for(address_t addr = 0; addr < 0x8000000; addr += 0x1000)
		mapSingle4kPage(addr, addr, kAccessWrite | kAccessExecute);

	mapRegionsAndStructs();

	// Setup the kernel image.
	loadKernelImage(kernel_start, &kernel_entry);
	frigg::infoLogger() << "eir: Allocated " << (allocatedMemory >> 10) << " KiB"
			" after loading the kernel" << frigg::endLog;

	// Setup the kernel stack.
	for(address_t page = 0; page < 0x10000; page += kPageSize)
		mapSingle4kPage(0xFFFF'FE80'0000'0000 + page, allocPage(), kAccessWrite);
}

EirInfo *generateInfo(const char* cmdline){
	// Setup the eir interface struct.
	auto info_ptr = bootAlloc<EirInfo>();
	memset(info_ptr, 0, sizeof(EirInfo));
	auto info_vaddr = mapBootstrapData(info_ptr);
	assert(info_vaddr == 0x40000000);
	info_ptr->signature = eirSignatureValue;

	// Pass all memory regions to thor.
	int n = 0;
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType == RegionType::allocatable)
			n++;
	}

	auto regionInfos = bootAllocN<EirRegion>(n);
	info_ptr->numRegions = n;
	info_ptr->regionInfo = mapBootstrapData(regionInfos);
	int j = 0;
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::allocatable)
			continue;

		regionInfos[j].address = regions[i].address;
		regionInfos[j].length = regions[i].size;
		regionInfos[j].order = regions[i].order;
		regionInfos[j].numRoots = regions[i].numRoots;
		regionInfos[j].buddyTree = regions[i].buddyMap;
		j++;
	}

	// Parse the kernel command line.
	const char *l = cmdline;
	while(true) {
		while(*l && *l == ' ')
			l++;
		if(!(*l))
			break;

		const char *s = l;
		while(*s && *s != ' ')
			s++;

		frigg::StringView token{l, static_cast<size_t>(s - l)};
		if(token == "serial") {
			info_ptr->debugFlags |= eirDebugSerial;
		}else if(token == "bochs") {
			info_ptr->debugFlags |= eirDebugBochs;
		}
		l = s;
	}

	auto cmd_length = strlen(cmdline);
	assert(cmd_length <= kPageSize);
	auto cmd_buffer = bootAllocN<char>(cmd_length);
	memcpy(cmd_buffer, cmdline, cmd_length + 1);
	info_ptr->commandLine = mapBootstrapData(cmd_buffer);
	
	return info_ptr;
}
