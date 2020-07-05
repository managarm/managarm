#pragma once

#include <async/basic.hpp>
#include <thor-internal/stream.hpp>

namespace thor {

void fiberCopyToBundle(MemoryView *bundle, ptrdiff_t offset, const void *pointer, size_t size);
void fiberCopyFromBundle(MemoryView *bundle, ptrdiff_t offset, void *pointer, size_t size);

} // namespace thor
