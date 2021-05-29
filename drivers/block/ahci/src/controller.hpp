#pragma once

#include <arch/mem_space.hpp>
#include <async/result.hpp>
#include <helix/memory.hpp>
#include <protocols/hw/client.hpp>

#include "port.hpp"

class Controller {

public:
	Controller(int64_t parentId, protocols::hw::Device hwDevice, helix::Mapping hbaRegs,
			helix::UniqueDescriptor ahciBar, helix::UniqueDescriptor irq);

	async::detached run();

private:
	async::result<bool> initPorts_(size_t numCommandSlots, bool staggeredSpinUp);
	async::detached handleIrqs_();

private:
	protocols::hw::Device hwDevice_;
	helix::Mapping regsMapping_;
	arch::mem_space regs_;
	helix::UniqueDescriptor irq_;

	std::vector<std::unique_ptr<Port>> activePorts_;

	int64_t parentId_;
	uint32_t portsImpl_;
	uint64_t irqSequence_;
	int maxPorts_;
};
