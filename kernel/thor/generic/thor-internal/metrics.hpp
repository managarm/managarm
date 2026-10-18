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

// Shootdowns of user page spaces, the ones among them that had to IPI other CPUs,
// and the number of CPUs that these IPIs targeted (including offline CPUs that are skipped).
inline ostrace::Counter userShootdownsCounter{"thor.asid.shootdowns"};
inline ostrace::Counter remoteUserShootdownsCounter{"thor.asid.shootdowns.remote"};
inline ostrace::Counter userShootdownTargetsCounter{"thor.asid.shootdowns.targets"};
// Number of CPUs that PageSpace::retire() targeted with an IPI (including offline CPUs that are skipped).
inline ostrace::Counter retireTargetsCounter{"thor.asid.retire.targets"};
// ASID invalidations of lazy bindings that missed shootdowns, when they become active again.
inline ostrace::Counter staleBindingFlushesCounter{"thor.asid.stale-flushes"};

} // namespace thor
