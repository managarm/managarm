#pragma once

#include <optional>

#include <thor-internal/util.hpp>

namespace thor {

// Returns the TSC tick duration (in ns) that KVM uses for kvmclock, if thor runs on KVM.
std::optional<Pow2Fraction<Rounding::down>> getKvmclockTscTickDuration();

} // namespace thor
