#pragma once

#include <stdint.h>

#include <thor-internal/cpu-data.hpp>

namespace thor {

struct CpuInfo {
	uint64_t features{0};
	uint64_t bugs{0};
	uint64_t midr{0};
};

extern PerCpu<CpuInfo> cpuInfo;

} // namespace thor
