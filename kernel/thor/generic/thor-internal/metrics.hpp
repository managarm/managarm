#pragma once

#include <thor-internal/kernel-ostrace.hpp>

namespace thor {

// IPI counters. The architecture code counts IPIs when it actually raises them,
// i.e., not for offline CPUs that it skips or for IPIs that are already pending.
inline ostrace::Counter pingIpisSentCounter{"thor.ipi.ping.sent"};
inline ostrace::Counter pingIpisReceivedCounter{"thor.ipi.ping.received"};
inline ostrace::Counter shootdownIpisSentCounter{"thor.ipi.shootdown.sent"};
inline ostrace::Counter shootdownIpisReceivedCounter{"thor.ipi.shootdown.received"};
inline ostrace::Counter selfCallIpisSentCounter{"thor.ipi.self-call.sent"};
inline ostrace::Counter selfCallIpisReceivedCounter{"thor.ipi.self-call.received"};
// Pings delivered by tryPingIdle() instead of an IPI.
inline ostrace::Counter pingIpisElidedCounter{"thor.ipi.ping.elided"};

} // namespace thor
