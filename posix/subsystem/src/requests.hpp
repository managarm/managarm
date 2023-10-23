#pragma once

#include "process.hpp"

async::result<void> serveRequests(std::shared_ptr<Process> self,
		std::shared_ptr<Generation> generation);

helix::UniqueLane &getKerncfgLane();
