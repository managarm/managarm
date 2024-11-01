#pragma once

namespace eir {

// Integer type large enough to hold physical and virtal addresses of the architecture.
using address_t = uint64_t;

void debugPrintChar(char c);

void setupPaging();
void mapSingle4kPage(
    uint64_t address,
    uint64_t physical,
    uint32_t flags,
    CachingMode caching_mode = CachingMode::null
);

} // namespace eir
