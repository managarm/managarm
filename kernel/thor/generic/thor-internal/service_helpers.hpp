#pragma once

#include <async/basic.hpp>
#include <thor-internal/stream.hpp>

namespace thor {

void fiberCopyToBundle(MemoryView *bundle, ptrdiff_t offset, const void *pointer, size_t size);
void fiberCopyFromBundle(MemoryView *bundle, ptrdiff_t offset, void *pointer, size_t size);

void fiberSleep(uint64_t nanos);

LaneHandle fiberOffer(LaneHandle lane);
LaneHandle fiberAccept(LaneHandle lane);

void fiberSend(LaneHandle lane, const void *buffer, size_t length);
frigg::UniqueMemory<KernelAlloc> fiberRecv(LaneHandle lane);

void fiberPushDescriptor(LaneHandle lane, AnyDescriptor descriptor);
AnyDescriptor fiberPullDescriptor(LaneHandle lane);

} // namespace thor
