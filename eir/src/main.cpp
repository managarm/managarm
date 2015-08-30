
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>
#include <frigg/array.hpp>
#include <frigg/elf.hpp>
#include <frigg/arch_x86/machine.hpp>
#include <frigg/arch_x86/gdt.hpp>
#include <frigg/libc.hpp>
#include <frigg/support.hpp>
#include <eir/interface.hpp>

namespace debug = frigg::debug;
namespace util = frigg::util;
namespace arch = frigg::arch_x86;

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

typedef debug::DefaultLogger<BochsSink> InfoLogger;
BochsSink infoSink;
util::LazyInitializer<InfoLogger> infoLogger;

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

uint64_t bootstrapPointer;
uint64_t bootstrapLimit;
uint64_t bootstrapBase;

void bootAlign(size_t alignment) {
	if((bootstrapPointer % alignment) != 0)
		bootstrapPointer += alignment - (bootstrapPointer % alignment);
	ASSERT(bootstrapPointer <= bootstrapLimit);
}

uintptr_t bootReserve(size_t length, size_t alignment) {
	bootAlign(alignment);
	uintptr_t pointer = bootstrapPointer;
	bootstrapPointer += length;
	ASSERT(bootstrapPointer <= bootstrapLimit);
	return pointer;
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

void mapSingle4kPage(uint64_t address, uint64_t physical, uint32_t flags) {
	ASSERT(address % 0x1000 == 0);
	ASSERT(physical % 0x1000 == 0);

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
	ASSERT((pt_entry & kPagePresent) == 0);
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
		debug::panicLogger.log() << "Illegal magic fields" << debug::Finish();
	}
	ASSERT(ehdr->e_type == ET_EXEC);
	
	for(int i = 0; i < ehdr->e_phnum; i++) {
		Elf64_Phdr *phdr = (Elf64_Phdr *)((uintptr_t)image
				+ (uintptr_t)ehdr->e_phoff
				+ i * ehdr->e_phentsize);
		ASSERT((phdr->p_offset % 0x1000) == 0);
		ASSERT((phdr->p_paddr % 0x1000) == 0);
		ASSERT(phdr->p_filesz == phdr->p_memsz);
		
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
			debug::panicLogger.log() << "Illegal combination of segment permissions"
					<< debug::Finish();
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
	infoLogger.initialize(infoSink);

	infoLogger->log() << "Starting Eir" << debug::Finish();

	util::Array<uint32_t, 4> vendor_res = arch::cpuid(0);
	char vendor_str[13];
	memcpy(&vendor_str[0], &vendor_res[1], 4);
	memcpy(&vendor_str[4], &vendor_res[3], 4);
	memcpy(&vendor_str[8], &vendor_res[2], 4);
	vendor_str[12] = 0;
	infoLogger->log() << "CPU vendor: " << (const char *)vendor_str << debug::Finish();
	
	// make sure everything we require is supported by the CPU
	util::Array<uint32_t, 4> extended = arch::cpuid(arch::kCpuIndexExtendedFeatures);
	if((extended[3] & arch::kCpuFlagLongMode) == 0)
		debug::panicLogger.log() << "Long mode is not supported on this CPU" << debug::Finish();
	if((extended[3] & arch::kCpuFlagNx) == 0)
		debug::panicLogger.log() << "NX bit is not supported on this CPU" << debug::Finish();
	
	// compute the bootstrap memory base
	ASSERT((mb_info->flags & kMbInfoPlainMemory) != 0);
	bootstrapPointer = (uintptr_t)&eirRtImageCeiling;
	bootstrapLimit = 0x100000 + (uint64_t)mb_info->memUpper * 1024;

	// make sure we don't trash boot modules
	if((mb_info->flags & kMbInfoModules) != 0) {
		for(unsigned int i = 0; i < mb_info->numModules; i++) {
			uintptr_t ceil = (uintptr_t)mb_info->modulesPtr[i].endAddress;
			if(ceil > bootstrapPointer)
				bootstrapPointer = ceil;
		}
	}
	bootAlign(0x1000);
	
	infoLogger->log() << "Bootstrap memory at "
			<< (void *)bootstrapPointer << ", length: "
			<< mb_info->memUpper << " KiB" << debug::Finish();

	// allocate a stack for the real thor kernel
	size_t thor_stack_length = 0x100000;
	uintptr_t thor_stack_base = bootReserve(thor_stack_length, 0x1000);
	
	// setup the bootstrap base AFTER allocating the stack
	// so that thor doesn't free its stack when freeing the bootstrap area
	bootAlign(0x1000);
	bootstrapBase = bootstrapPointer;
	
	ASSERT((mb_info->flags & kMbInfoMemoryMap) != 0);
	infoLogger->log() << "Memory map:" << debug::Finish();
	size_t offset = 0;
	while(offset < mb_info->memoryMapLength) {
		MbMemoryMap *map = (MbMemoryMap *)((uintptr_t)mb_info->memoryMapPtr
				+ offset);
		
		if(map->type == 1)
			infoLogger->log() << "   Base: " << (void *)map->baseAddress
					<< ", length: " << (map->length / 1024) << " KiB" << debug::Finish();

		offset += map->size + 4;
	}

	intializeGdt();
	setupPaging();

	// setup the eir interface struct
	auto info = bootAlloc<EirInfo>();

	// identically map the first 128 mb so that
	// we can activate paging without causing a page fault
	for(uint64_t addr = 0; addr < 0x8000000; addr += 0x1000)
		mapSingle4kPage(addr, addr, kAccessWrite | kAccessExecute);

	// TODO: move to a global configuration file
	uint64_t physical_window = 0xFFFF800100000000;
	
	// map physical memory into kernel virtual memory
	for(uint64_t addr = 0; addr < 0x100000000; addr += 0x1000)
		mapSingle4kPage(physical_window + addr, addr, kAccessWrite);
	
	ASSERT((mb_info->flags & kMbInfoModules) != 0);
	ASSERT(mb_info->numModules >= 2);
	MbModule *kernel_module = &mb_info->modulesPtr[0];

	uint64_t kernel_entry;
	loadKernelImage(kernel_module->startAddress, &kernel_entry);

	// setup the module information
	auto modules = bootAllocN<EirModule>(mb_info->numModules - 1);
	for(size_t i = 0; i < mb_info->numModules - 1; i++) {
		MbModule &image_module = mb_info->modulesPtr[i + 1];
		modules[i].physicalBase = (EirPtr)image_module.startAddress;
		modules[i].length = (EirPtr)image_module.endAddress;
				- (EirPtr)image_module.startAddress;
		
		size_t name_length = strlen(image_module.string);
		char *name_ptr = bootAllocN<char>(name_length);
		memcpy(name_ptr, image_module.string, name_length);
		modules[i].namePtr = (EirPtr)name_ptr;
		modules[i].nameLength = name_length;
	}
	info->numModules = mb_info->numModules - 1;
	info->moduleInfo = (EirPtr)modules;

	// finalize the eir information struct
	bootAlign(0x1000);
	ASSERT((bootstrapLimit % 0x1000) == 0);
	info->bootstrapPhysical = bootstrapPointer;
	info->bootstrapLength = bootstrapLimit - bootstrapPointer;

	infoLogger->log() << "Leaving Eir and entering the real kernel" << debug::Finish();
	eirRtEnterKernel(eirPml4Pointer, kernel_entry,
			physical_window + thor_stack_base + thor_stack_length, info);
}

