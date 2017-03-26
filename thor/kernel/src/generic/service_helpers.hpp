#ifndef THOR_GENERIC_SERVICE_HELPERS_HPP
#define THOR_GENERIC_SERVICE_HELPERS_HPP

#include "stream.hpp"

namespace thor {

void fiberSleep(uint64_t nanos);

LaneHandle fiberOffer(LaneHandle lane);
LaneHandle fiberAccept(LaneHandle lane);

void fiberSend(LaneHandle lane, const void *buffer, size_t length);
frigg::UniqueMemory<KernelAlloc> fiberRecv(LaneHandle lane);

void fiberPushDescriptor(LaneHandle lane, AnyDescriptor descriptor);
AnyDescriptor fiberPullDescriptor(LaneHandle lane);

} // namespace thor

#endif // THOR_GENERIC_SERVICE_HELPERS_HPP
