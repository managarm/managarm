#pragma once

#include <hel.h>

#include <thor-internal/virtualization.hpp>

namespace thor {

struct X86VirtualizedCpu : VirtualizedCpu {
	virtual void storeBasicRegs(const HelX86VirtualizationBasicRegs *regs) = 0;
	virtual void loadBasicRegs(HelX86VirtualizationBasicRegs *regs) = 0;

	virtual bool storeMsrs(const HelX86VirtualizationMsrs *msrs) = 0;
	virtual void loadMsrs(HelX86VirtualizationMsrs *msrs) = 0;
};

} // namespace thor
