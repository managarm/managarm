#pragma once

#include <frg/dyn_bitset.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/kernel-heap.hpp>

namespace thor {

struct CpuData;

void sendPingIpi(CpuData *dstData);
void sendShootdownIpi();
// Targets only the given CPUs. The target set may include the current CPU.
void sendShootdownIpi(const frg::dyn_bitset<KernelAlloc> &targets);
void sendSelfCallIpi();

} // namespace thor
