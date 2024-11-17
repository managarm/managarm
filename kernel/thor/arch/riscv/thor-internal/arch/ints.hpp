#pragma once

#include <assert.h>
#include <frg/spinlock.hpp>
#include <thor-internal/arch/unimplemented.hpp>
#include <thor-internal/debug.hpp>

namespace thor {

void initializeIrqVectors();

inline bool intsAreEnabled() { unimplementedOnRiscv(); }

inline void enableInts() { unimplementedOnRiscv(); }

inline void disableInts() { unimplementedOnRiscv(); }

inline void halt() { unimplementedOnRiscv(); }

inline void suspendSelf() { unimplementedOnRiscv(); }

inline void sendPingIpi(int id) { unimplementedOnRiscv(); }

inline void sendShootdownIpi() { unimplementedOnRiscv(); }

} // namespace thor
