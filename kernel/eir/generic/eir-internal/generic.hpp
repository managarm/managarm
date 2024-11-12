#pragma once

#include <stdint.h>
#include <stddef.h>
#include <eir-internal/arch/types.hpp>
#include <frg/string.hpp>
#include <frg/span.hpp>
#include <eir/interface.hpp>

namespace eir {

extern frg::span<uint8_t> kernel_image;
extern frg::span<uint8_t> initrd_image;

enum class RegionType {
	null,
	unconstructed,
	allocatable
};

struct Region {
	RegionType regionType;
	address_t address;
	address_t size;

	int order;
	uint64_t numRoots;
	address_t buddyTree;
	address_t buddyOverhead;
	address_t buddyMap;
};

static constexpr size_t numRegions = 64;
extern Region regions[numRegions];
extern address_t allocatedMemory;

struct GenericInfo {
	uintptr_t deviceTreePtr;
	const char *cmdline;
	EirFramebuffer fb;
	uint32_t debugFlags;
	bool hasFb;
};

void eirRelocate();
[[noreturn]] void eirGenericMain(const GenericInfo &genericInfo);

uintptr_t bootReserve(size_t length, size_t alignment);
uintptr_t allocPage();
void allocLogRingBuffer();

void setupRegionStructs();
void createInitialRegion(address_t base, address_t size);

struct InitialRegion {
	address_t base;
	address_t size;
};

void createInitialRegions(InitialRegion region, frg::span<InitialRegion> reserved);

address_t mapBootstrapData(void *p);
void mapKasanShadow(uint64_t address, size_t size);
void unpoisonKasanShadow(uint64_t address, size_t size);
void mapRegionsAndStructs();

// Kernel entrypoint.
extern uint64_t kernelEntry;

void parseInitrd(void *initrd);
address_t loadKernelImage(void *image);

EirInfo *generateInfo(frg::string_view cmdline);

void setFbInfo(void *ptr, int width, int height, size_t pitch);

template<typename T>
T *bootAlloc(size_t n = 1) {
	auto pointer = reinterpret_cast<T *>(bootReserve(sizeof(T) * n, alignof(T)));
	for(size_t i = 0; i < n; i++)
		new (&pointer[i]) T();
	return pointer;
}

} // namespace eir
