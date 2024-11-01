#pragma once

#include <eir-internal/arch/types.hpp>
#include <stddef.h>
#include <stdint.h>

namespace eir {

void debugPrintChar(char c);

// read and privileged/supervisor is implied
namespace PageFlags {
static inline constexpr uint32_t write = 1;
static inline constexpr uint32_t execute = 2;
static inline constexpr uint32_t global = 4;
} // namespace PageFlags

enum class CachingMode { null, writeCombine, mmio }; // enum class CachingMode

static constexpr int pageShift = 12;
static constexpr size_t pageSize = size_t(1) << pageShift;

void setupPaging();
void mapSingle4kPage(
    address_t address,
    address_t physical,
    uint32_t flags,
    CachingMode caching_mode = CachingMode::null
);
address_t getSingle4kPage(address_t address);

void initProcessorEarly();
void initProcessorPaging(void *kernel_start, uint64_t &kernel_entry);

// These need to be hidden because they are used in eirRelocate.
extern "C" [[gnu::visibility("hidden")]] char eirImageFloor;
extern "C" [[gnu::visibility("hidden")]] char eirImageCeiling;

} // namespace eir
