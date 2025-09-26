#pragma once

namespace thor {

static inline constexpr int numIrqSlots = 0;

void initializeArchitecture();
bool isKernelInEl2();

} // namespace thor
