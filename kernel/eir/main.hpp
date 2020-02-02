#pragma once

#include <stdint.h>
#include <stddef.h>

enum {
	kAccessWrite = 1,
	kAccessExecute = 2,
	kAccessGlobal = 4,
};

enum class CachingMode {
	null,
	writeCombine
};

enum class RegionType {
	null,
	unconstructed,
	allocatable
};

// Integer type large enough to hold physical and virtal addresses of the architecture.
using address_t = uint64_t;

struct Region {
	RegionType regionType;
	address_t address;
	address_t size;

	int order;
	uint64_t numRoots;
	address_t buddyTree;
	address_t buddyOverhead;
	//uint64_t pageStructs;
	address_t buddyMap;
};

static constexpr int kPageShift = 12;
static constexpr size_t kPageSize = size_t(1) << kPageShift;
static constexpr size_t numRegions = 64;

uintptr_t bootReserve(size_t length, size_t alignment);
address_t mapBootstrapData(void *p);
void mapSingle4kPage(uint64_t address, uint64_t physical, uint32_t flags, CachingMode caching_mode);
void initProcessorEarly();
void setupRegionStructs();
void createInitialRegion(address_t base, address_t size);
void initProcessorPaging(void *kernel_start, uint64_t& kernel_entry);
EirInfo *generateInfo(const char* cmdline);

extern "C" void eirRtEnterKernel(uint32_t pml4, uint64_t entry, uint64_t stack_ptr);

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

extern char eirRtImageCeiling;
extern address_t bootMemoryLimit;
extern uintptr_t eirPml4Pointer;

extern void *displayFb;
extern int displayWidth;
extern int displayHeight;
extern int displayPitch;

extern Region regions[numRegions];
