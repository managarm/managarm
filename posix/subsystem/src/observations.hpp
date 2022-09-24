#pragma once

#include "process.hpp"

async::result<void> observeThread(std::shared_ptr<Process> self,
		std::shared_ptr<Generation> generation);
