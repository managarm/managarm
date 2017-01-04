
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

// ----------------------------------------------------------------------------
// Memory region management.
// ----------------------------------------------------------------------------

enum class RegionType {
	null,
	reserved,
	allocatable,
	buddy
};

struct Region {
	RegionType regionType;
	uint64_t address;
	uint64_t size;

	union {
		struct {
			int order;
			uint64_t numRoots;
		};
		struct {
			Region *buddy;
		};
	};
};

static constexpr size_t numRegions = 1024;

Region regions[numRegions];

Region *obtainRegion() {
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::null)
			continue;
		regions[i].regionType = RegionType::reserved;
		return &regions[i];
	}
	frigg::panicLogger() << "Eir: Memory region limit exhausted" << frigg::endLog;
}

void cutMemoryIntoRegions(uint64_t address, uint64_t size) {
	auto limit = address + size;

	// For now we do not touch memory that is required during boot.
	address = frigg::max(address, bootMemoryLimit);
	
	// Align address to 2 MiB.
	// This ensures thor can allocate contiguous chunks of up to 2 MiB.
	address = (address + 0x1FFFFF) & ~uint64_t(0x1FFFFF);

	if(address >= limit)
		return;
	// TODO: Handle small memory regions.
	assert(limit - address >= 32 * uint64_t(0x100000));

	assert(!(address % kPageSize));
	assert(!(limit % kPageSize));

	// Setup a buddy allocator.
	auto order = frigg::buddy_tools::suitable_order((limit - address) >> kPageShift);
	auto pre_roots = (limit - address) >> (kPageShift + order);
	auto overhead = frigg::buddy_tools::determine_size(pre_roots, order);
	overhead = (overhead + uint64_t(kPageSize - 1)) & ~uint64_t(kPageSize - 1);
	assert(overhead < limit - address);

	// Setup the memory regions.
	auto main_region = obtainRegion();
	auto buddy_region = obtainRegion();

	main_region->regionType = RegionType::allocatable;
	main_region->address = address;
	main_region->size = limit - address - overhead;
	main_region->buddy = buddy_region;
	
	buddy_region->regionType = RegionType::buddy;
	buddy_region->address = limit - overhead;
	buddy_region->size = overhead;
	buddy_region->order = order;
	buddy_region->numRoots = (limit - address - overhead) >> (kPageShift + order);
	assert(buddy_region->numRoots >= 32);

	// Finally initialize the buddy tree.
	auto table = reinterpret_cast<int8_t *>(buddy_region->address);
	frigg::buddy_tools::initialize(table, buddy_region->numRoots, buddy_region->order);
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
	kPageXd = 0x8000000000000000
};

uintptr_t bootReserve(size_t length, size_t alignment) {
	assert(length <= kPageSize);
	assert(alignment <= kPageSize);

	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType != RegionType::allocatable)
			continue;

		auto table = reinterpret_cast<int8_t *>(regions[i].buddy->address);
		auto physical = regions[i].address + (frigg::buddy_tools::allocate(table,
				regions[i].buddy->numRoots, regions[i].buddy->order, 0) << kPageShift);
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
	return (uintptr_t)bootReserve(0x1000, 0x1000);
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
	uint64_t pd_entry = ((uint64_t*)pd)[pd_index];
	
	// find the pt entry; create pt if necessary
	uintptr_t pt = (uintptr_t)(pd_entry & 0xFFFFF000);
	if((pd_entry & kPagePresent) == 0) {
		pt = allocPage();
		for(int i = 0; i < 512; i++)
			((uint64_t*)pt)[i] = 0;
		((uint64_t*)pd)[pd_index] = pt | kPagePresent | kPageWrite;
	}
	uint64_t pt_entry = ((uint64_t*)pt)[pt_index];
	
	// setup the new pt entry
	assert((pt_entry & kPagePresent) == 0);
	uint64_t new_entry = physical | kPagePresent;
	if((flags & kAccessWrite) != 0)
		new_entry |= kPageWrite;
	if((flags & kAccessExecute) == 0)
		new_entry |= kPageXd;
	((uint64_t*)pt)[pt_index] = new_entry;
}

extern char eirRtImageCeiling;
extern "C" void eirRtLoadGdt(uintptr_t gdt_page, uint32_t size);
extern "C" void eirRtEnterKernel(uint32_t pml4, uint64_t entry,
		uint64_t stack_ptr, EirInfo *info);

void intializeGdt() {
	uintptr_t gdt_page = allocPage();
	arch::makeGdtNullSegment((uint32_t *)gdt_page, 0);
	arch::makeGdtFlatCode32SystemSegment((uint32_t *)gdt_page, 1);
	arch::makeGdtFlatData32SystemSegment((uint32_t *)gdt_page, 2);
	arch::makeGdtCode64SystemSegment((uint32_t *)gdt_page, 3);
	
	eirRtLoadGdt(gdt_page, 31); 
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
		assert(phdr->p_filesz == phdr->p_memsz);
		
		if(phdr->p_type != PT_LOAD)
			continue;

		uint32_t map_flags = 0;
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

		uint32_t page = 0;
		while(page < (uint32_t)phdr->p_filesz) {
			mapSingle4kPage(phdr->p_paddr + page,
					(uintptr_t)image + (uint32_t)phdr->p_offset + page, map_flags);
			page += 0x1000;
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
	void *commandLine;
	uint32_t numModules;
	MbModule *modulesPtr;
	uint32_t numSymbols;
	uint32_t symbolSize;
	void *symbolsPtr;
	uint32_t stringSection;
	uint32_t memoryMapLength;
	void *memoryMapPtr;
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
			cutMemoryIntoRegions(map->baseAddress, map->length);

		offset += map->size + 4;
	}
	
	frigg::infoLogger() << "Kernel memory regions:" << frigg::endLog;
	for(size_t i = 0; i < numRegions; ++i) {
		if(regions[i].regionType == RegionType::null)
			continue;
		frigg::infoLogger() << "    Type " << (int)regions[i].regionType << " region."
				<< " Base: " << (void *)regions[i].address
				<< ", length: " << (void *)regions[i].size << frigg::endLog;
	}

	intializeGdt();
	setupPaging();

	// identically map the first 128 mb so that
	// we can activate paging without causing a page fault
	for(uint64_t addr = 0; addr < 0x8000000; addr += 0x1000)
		mapSingle4kPage(addr, addr, kAccessWrite | kAccessExecute);

	// TODO: move to a global configuration file
	uint64_t physical_window = 0xFFFF800100000000;
	
	// map physical memory into kernel virtual memory
	for(uint64_t addr = 0; addr < 0x100000000; addr += 0x1000)
		mapSingle4kPage(physical_window + addr, addr, kAccessWrite);
	
	assert((mb_info->flags & kMbInfoModules) != 0);
	assert(mb_info->numModules >= 2);
	MbModule *kernel_module = &mb_info->modulesPtr[0];

	uint64_t kernel_entry;
	loadKernelImage(kernel_module->startAddress, &kernel_entry);

	// Setup the kernel stack.
	for(size_t page = 0; page < 0x10000; page += kPageSize)
		mapSingle4kPage(0xFFFF'FE80'0000'0000 + page, allocPage(), kAccessWrite);

	// Setup the buddy allocator window.
	assert(regions[0].regionType == RegionType::allocatable);
	assert(regions[1].regionType == RegionType::buddy);
	assert(regions[0].buddy == &regions[1]);

	for(size_t page = 0; page < regions[1].size; page += kPageSize)
		mapSingle4kPage(0xFFFF'FF00'0000'0000 + page,
				regions[1].address + page, kAccessWrite);

	// finally setup the BSPs physical windows.
	auto physical1 = allocPt();
	auto physical2 = allocPt();
	mapSingle4kPage(0xFFFF'FF80'0000'1000, physical1, kAccessWrite);
	mapSingle4kPage(0xFFFF'FF80'0000'2000, physical2, kAccessWrite);
	mapPt(0xFFFF'FF80'0020'0000, physical1);
	mapPt(0xFFFF'FF80'0040'0000, physical2);
	
	// Setup the eir interface struct.
	auto info = bootAlloc<EirInfo>();
	info->address = regions[0].address;
	info->length = regions[0].size;
	info->order = regions[1].order;
	info->numRoots = regions[1].numRoots;

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
		modules[i].namePtr = (EirPtr)name_ptr;
		modules[i].nameLength = name_length;
	}
	info->numModules = mb_info->numModules - 1;
	info->moduleInfo = (EirPtr)modules;

	frigg::infoLogger() << "Leaving Eir and entering the real kernel" << frigg::endLog;
	eirRtEnterKernel(eirPml4Pointer, kernel_entry,
			0xFFFF'FE80'0001'0000, info);
}

