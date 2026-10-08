#pragma once

#include "process.hpp"

struct ProcfsCpuInfo {
	uint64_t features;
	uint64_t bugs;
	uint64_t midr;
};

async::result<void> serveRequests(std::shared_ptr<Process> self,
		std::shared_ptr<Generation> generation);

helix::UniqueLane &getKerncfgLane();
helix::UniqueLane &getPmLane();

size_t getAffinityMaskSize();
size_t getProcfsCpuCount();
const ProcfsCpuInfo &getProcfsCpuInfo(size_t cpu);
