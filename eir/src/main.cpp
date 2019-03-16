
#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>
#include <frigg/array.hpp>
#include <frigg/elf.hpp>
#include <frigg/arch_x86/machine.hpp>
#include <frigg/arch_x86/gdt.hpp>
#include <frigg/libc.hpp>
#include <frigg/support.hpp>
#include <frigg/physical_buddy.hpp>
#include <eir/interface.hpp>

namespace arch = frigg::arch_x86;

static constexpr int kPageShift = 12;
static constexpr size_t kPageSize = size_t(1) << kPageShift;

uint64_t bootMemoryLimit;
uint64_t allocatedMemory;

// ----------------------------------------------------------------------------
// Memory region management.
// ----------------------------------------------------------------------------

enum class RegionType {
	null,
	unconstructed,
	allocatable
};

struct Region {
	RegionType regionType;
	uint64_t address;
	uint64_t size;

	int order;
	uint64_t numRoots;
	uint64_t buddyTree;
	uint64_t buddyOverhead;
	//uint64_t pageStructs;
	uint64_t buddyMap;
};

static constexpr size_t numRegions = 64;

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

void createInitialRegion(uint64_t address, uint64_t size) {
	auto limit = address + size;

	// For now we do not touch memory that is required during boot.
	address = frigg::max(address, bootMemoryLimit);
	
	// Align address to 2 MiB.
	// This ensures thor can allocate contiguous chunks of up to 2 MiB.
	address = (address + 0x1FFFFF) & ~uint64_t(0x1FFFFF);

	if(address >= limit)
		return;
	
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
	assert(limit - address >= 32 * uint64_t(0x100000));

	assert(!(address % kPageSize));
	assert(!(limit % kPageSize));

	auto region = obtainRegion();
	region->regionType = RegionType::allocatable;
	region->address = address;
	region->size = limit - address;
}

uint64_t cutFromRegion(size_t size) {
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
		auto order = frigg::buddy_tools::suitable_order(regions[i].size >> kPageShift);
		auto pre_roots = regions[i].size >> (kPageShift + order);
		auto overhead = frigg::buddy_tools::determine_size(pre_roots, order);
		overhead = (overhead + uint64_t(kPageSize - 1)) & ~uint64_t(kPageSize - 1);

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
		frigg::buddy_tools::initialize(table_ptr, num_roots, order);

		// Setup the struct Page area.
		// TODO: It is not clear that we want page structs. Remove this?
		//auto num_pages = regions[i].size >> kPageShift;
		//auto structs_size = (num_pages * 32 + uint64_t(kPageSize - 1))
		//		& ~(uint64_t(kPageSize - 1));
		//regions[i].pageStructs = cutFromRegion(structs_size);
	}
}

// ----------------------------------------------------------------------------

class BochsSink {
public:
	void print(char c);
	void print(const char *str);
};

void BochsSink::print(char c) {
	arch::ioOutByte(0xE9, c);
}
void BochsSink::print(const char *str) {
	while(*str != 0)
		arch::ioOutByte(0xE9, *str++);
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
		auto physical = regions[i].address + (frigg::buddy_tools::allocate(table,
				regions[i].numRoots, regions[i].order, 0) << kPageShift);
//		frigg::infoLogger() << "Allocate " << (void *)physical << frigg::endLog;
		return physical;
	}

	frigg::panicLogger() << "Eir: Out of memory" << frigg::endLog;
}

template<typename T>
T *bootAlloc() {
	return new ((void *)bootReserve(sizeof(T), alignof(T))) T();
}

template<typename T>
T *bootAllocN(int n) {
	auto pointer = (T *)bootReserve(sizeof(T) * n, alignof(T));
	for(size_t i = 0; i < n; i++)
		new (&pointer[i]) T();
	return pointer;
}

uintptr_t allocPage() {
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::allocatable)
			continue;

		auto table = reinterpret_cast<int8_t *>(regions[i].buddyTree);
		auto physical = regions[i].address + (frigg::buddy_tools::allocate(table,
				regions[i].numRoots, regions[i].order, 0) << kPageShift);
		allocatedMemory += kPageSize;
	//		frigg::infoLogger() << "Allocate " << (void *)physical << frigg::endLog;
		return physical;
	}
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

enum {
	kAccessWrite = 1,
	kAccessExecute = 2,
	kAccessGlobal = 4,
};

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
	assert(!((uint64_t *)pd)[pd_index] & kPagePresent);
	((uint64_t *)pd)[pd_index] = pt | kPagePresent | kPageWrite;
}

void mapSingle4kPage(uint64_t address, uint64_t physical, uint32_t flags) {
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
	assert(!(pt_entry & kPagePresent));
	uint64_t new_entry = physical | kPagePresent;
	if(flags & kAccessWrite)
		new_entry |= kPageWrite;
	if(!(flags & kAccessExecute))
		new_entry |= kPageXd;
	if(!(flags & kAccessGlobal))
		new_entry |= kPageGlobal;
	((uint64_t*)pt)[pt_index] = new_entry;
}

// ----------------------------------------------------------------------------

void mapRegionsAndStructs() {
	// This region should be available RAM on every PC.
	for(size_t page = 0x8000; page < 0x80000; page += kPageSize)
			mapSingle4kPage(0xFFFF'8000'0000'0000 + page,
					page, kAccessWrite | kAccessGlobal);

	uint64_t tree_mapping = 0xFFFF'C080'0000'0000;
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::allocatable)
			continue;

		// Map the region itself.
		for(size_t page = 0; page < regions[i].size; page += kPageSize)
			mapSingle4kPage(0xFFFF'8000'0000'0000 + regions[i].address + page,
					regions[i].address + page, kAccessWrite | kAccessGlobal);

		// Map the buddy tree.
		regions[i].buddyMap = tree_mapping;

		auto overhead = frigg::buddy_tools::determine_size(regions[i].numRoots, regions[i].order);
		for(size_t page = 0; page < overhead; page += kPageSize) {
			mapSingle4kPage(tree_mapping, regions[i].buddyTree + page, kAccessWrite | kAccessGlobal);
			tree_mapping += kPageSize;
		}
	}
}

// ----------------------------------------------------------------------------
// Bootstrap information handling.
// ----------------------------------------------------------------------------

uint64_t bootstrapDataPointer = 0x40000000;

uint64_t mapBootstrapData(void *p) {
	auto pointer = bootstrapDataPointer;
	bootstrapDataPointer += kPageSize;
	mapSingle4kPage(pointer, (uint64_t)p, 0);
	return pointer;
}

// ----------------------------------------------------------------------------

extern char eirRtImageCeiling;
// TODO: eirRtLoadGdt could be written using inline assembly.
extern "C" void eirRtLoadGdt(uint32_t *pointer, uint32_t size);
extern "C" void eirRtEnterKernel(uint32_t pml4, uint64_t entry,
		uint64_t stack_ptr);

uint32_t gdtEntries[4 * 2];

void intializeGdt() {
	arch::makeGdtNullSegment(gdtEntries, 0);
	arch::makeGdtFlatCode32SystemSegment(gdtEntries, 1);
	arch::makeGdtFlatData32SystemSegment(gdtEntries, 2);
	arch::makeGdtCode64SystemSegment(gdtEntries, 3);
	
	eirRtLoadGdt(gdtEntries, 4 * 8 - 1); 
}

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

		uint32_t pg = 0;
		while(pg < (uint32_t)phdr->p_memsz) {
			auto backing = allocPage();
			memset(reinterpret_cast<void *>(backing), 0, kPageSize);
			if(pg < (uint32_t)phdr->p_filesz)
				memcpy(reinterpret_cast<void *>(backing),
						reinterpret_cast<void *>((uintptr_t)image + (uint32_t)phdr->p_offset + pg),
						frigg::min(kPageSize, (uint32_t)phdr->p_filesz - pg));
			mapSingle4kPage(phdr->p_paddr + pg, backing, map_flags);
			pg += kPageSize;
		}
	}
	
	*out_entry = ehdr->e_entry;
}

static_assert(sizeof(void *) == 4, "Expected 32-bit system");

enum MbInfoFlags {
	kMbInfoPlainMemory = 1,
	kMbInfoBootDevice = 2,
	kMbInfoCommandLine = 4,
	kMbInfoModules = 8,
	kMbInfoFramebuffer = 12,
	kMbInfoSymbols = 16,
	kMbInfoMemoryMap = 32
};

struct MbModule {
	void *startAddress;
	void *endAddress;
	char *string;
	uint32_t reserved;
};

struct MbInfo {
	uint32_t flags;
	uint32_t memLower;
	uint32_t memUpper;
	uint32_t bootDevice;
	char *commandLine;
	uint32_t numModules;
	MbModule *modulesPtr;
	uint32_t numSymbols;
	uint32_t symbolSize;
	void *symbolsPtr;
	uint32_t stringSection;
	uint32_t memoryMapLength;
	void *memoryMapPtr;
	uint32_t padding[9];
	uint64_t fbAddress;
	uint32_t fbPitch;
	uint32_t fbWidth;
	uint32_t fbHeight;
	uint8_t fbBpp;
	uint8_t fbType;
	uint8_t colorInfo[6];
};

struct MbMemoryMap {
	uint32_t size;
	uint64_t baseAddress;
	uint64_t length;
	uint32_t type;
};

extern "C" void eirMain(MbInfo *mb_info) {
	frigg::infoLogger() << "" << frigg::endLog;
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

	intializeGdt();
	
	// Make sure we do not trash ourselfs or our boot modules.
	bootMemoryLimit = (uintptr_t)&eirRtImageCeiling;

	if((mb_info->flags & kMbInfoModules) != 0) {
		for(unsigned int i = 0; i < mb_info->numModules; i++) {
			uintptr_t ceil = (uintptr_t)mb_info->modulesPtr[i].endAddress;
			if(ceil > bootMemoryLimit)
				bootMemoryLimit = ceil;
		}
	}

	bootMemoryLimit = (bootMemoryLimit + uint64_t(kPageSize - 1))
			& ~uint64_t(kPageSize - 1);

	// ------------------------------------------------------------------------
	// Memory region setup.
	// ------------------------------------------------------------------------

	// Walk the memory map and retrieve all useable regions.
	assert(mb_info->flags & kMbInfoMemoryMap);
	frigg::infoLogger() << "Memory map:" << frigg::endLog;
	size_t offset = 0;
	while(offset < mb_info->memoryMapLength) {
		auto map = (MbMemoryMap *)((uintptr_t)mb_info->memoryMapPtr + offset);
		
		frigg::infoLogger() << "    Type " << map->type << " mapping."
				<< " Base: " << (void *)map->baseAddress
				<< ", length: " << (void *)map->length << frigg::endLog;

		if(map->type == 1)
			createInitialRegion(map->baseAddress, map->length);

		offset += map->size + 4;
	}

	setupRegionStructs();
	
	frigg::infoLogger() << "Kernel memory regions:" << frigg::endLog;
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType == RegionType::null)
			continue;
		frigg::infoLogger() << "    Type " << (int)regions[i].regionType << " region."
				<< " Base: " << (void *)regions[i].address
				<< ", length: " << (void *)regions[i].size << frigg::endLog;
		if(regions[i].regionType == RegionType::allocatable)
			frigg::infoLogger() << "        Buddy tree at " << (void *)regions[i].buddyTree
					<< ", overhead: " << frigg::logHex(regions[i].buddyOverhead)
					<< frigg::endLog;
	}

	// ------------------------------------------------------------------------

	// Program the PAT. Each byte configures a single entry.
	// 00: Uncacheable
	// 01: Write Combining
	// 04: Write Through
	// 06: Write Back
	// Keep in sync with the SMP trampoline in thor.
	uint64_t pat = 0x00'00'01'00'00'00'04'06;
	frigg::arch_x86::wrmsr(0x277, pat);
	
	setupPaging();
	frigg::infoLogger() << "eir: Allocated " << (allocatedMemory >> 10) << " KiB"
			" after setting up paging" << frigg::endLog;

	// Identically map the first 128 MiB so that we can activate paging
	// without causing a page fault.
	for(uint64_t addr = 0; addr < 0x8000000; addr += 0x1000)
		mapSingle4kPage(addr, addr, kAccessWrite | kAccessExecute);

	mapRegionsAndStructs();

	// Setup the kernel image.
	assert((mb_info->flags & kMbInfoModules) != 0);
	assert(mb_info->numModules >= 2);
	MbModule *kernel_module = &mb_info->modulesPtr[0];

	uint64_t kernel_entry;
	loadKernelImage(kernel_module->startAddress, &kernel_entry);
	frigg::infoLogger() << "eir: Allocated " << (allocatedMemory >> 10) << " KiB"
			" after loading the kernel" << frigg::endLog;

	// Setup the kernel stack.
	for(size_t page = 0; page < 0x10000; page += kPageSize)
		mapSingle4kPage(0xFFFF'FE80'0000'0000 + page, allocPage(), kAccessWrite);

	// Setup the eir interface struct.
	auto info_ptr = bootAlloc<EirInfo>();
	memset(info_ptr, 0, sizeof(EirInfo));
	auto info_vaddr = mapBootstrapData(info_ptr);
	assert(info_vaddr == 0x40000000);
	info_ptr->signature = eirSignatureValue;
	info_ptr->coreRegion.address = regions[0].address;
	info_ptr->coreRegion.length = regions[0].size;
	info_ptr->coreRegion.order = regions[0].order;
	info_ptr->coreRegion.numRoots = regions[0].numRoots;
	info_ptr->coreRegion.buddyTree = regions[0].buddyMap;

	assert(mb_info->flags & kMbInfoCommandLine);
	auto cmd_length = strlen(mb_info->commandLine);
	assert(cmd_length <= kPageSize);
	auto cmd_buffer = bootAllocN<char>(cmd_length);
	memcpy(cmd_buffer, mb_info->commandLine, cmd_length + 1);
	info_ptr->commandLine = mapBootstrapData(cmd_buffer);

	// Setup the module information.
	auto modules = bootAllocN<EirModule>(mb_info->numModules - 1);
	for(size_t i = 0; i < mb_info->numModules - 1; i++) {
		MbModule &image_module = mb_info->modulesPtr[i + 1];
		modules[i].physicalBase = (EirPtr)image_module.startAddress;
		modules[i].length = (EirPtr)image_module.endAddress
				- (EirPtr)image_module.startAddress;

		size_t name_length = strlen(image_module.string);
		char *name_ptr = bootAllocN<char>(name_length);
		memcpy(name_ptr, image_module.string, name_length);
		modules[i].namePtr = mapBootstrapData(name_ptr);
		modules[i].nameLength = name_length;
	}
	info_ptr->numModules = mb_info->numModules - 1;
	info_ptr->moduleInfo = mapBootstrapData(modules);
	
	if((mb_info->flags & kMbInfoFramebuffer) != 0) {
		auto framebuf = &info_ptr->frameBuffer;
		framebuf->fbAddress = mb_info->fbAddress;
		framebuf->fbPitch = mb_info->fbPitch;
		framebuf->fbWidth = mb_info->fbWidth;
		framebuf->fbHeight = mb_info->fbHeight;
		framebuf->fbBpp = mb_info->fbBpp;
		framebuf->fbType = mb_info->fbType;
	
		// Map the framebuffer to a lower-half address.
		assert(mb_info->fbAddress & ~(kPageSize - 1));
		for(uint64_t pg = 0; pg < mb_info->fbPitch * mb_info->fbHeight; pg += 0x1000)
			mapSingle4kPage(0x80000000 + pg, mb_info->fbAddress + pg, kAccessWrite);
		framebuf->fbEarlyWindow = 0x80000000;
	}

	frigg::infoLogger() << "Leaving Eir and entering the real kernel" << frigg::endLog;
	eirRtEnterKernel(eirPml4Pointer, kernel_entry,
			0xFFFF'FE80'0001'0000);
}

