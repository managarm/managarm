#ifndef THOR_GENERIC_SERVICE_HELPERS_HPP
#define THOR_GENERIC_SERVICE_HELPERS_HPP

#include "stream.hpp"

namespace thor {

void fiberCopyToBundle(Memory *bundle, ptrdiff_t offset, const void *pointer, size_t size);
void fiberCopyFromBundle(Memory *bundle, ptrdiff_t offset, void *pointer, size_t size);

void fiberSleep(uint64_t nanos);

LaneHandle fiberOffer(LaneHandle lane);
LaneHandle fiberAccept(LaneHandle lane);

void fiberSend(LaneHandle lane, const void *buffer, size_t length);
frigg::UniqueMemory<KernelAlloc> fiberRecv(LaneHandle lane);

void fiberPushDescriptor(LaneHandle lane, AnyDescriptor descriptor);
AnyDescriptor fiberPullDescriptor(LaneHandle lane);

} // namespace thor

#endif // THOR_GENERIC_SERVICE_HELPERS_HPP
