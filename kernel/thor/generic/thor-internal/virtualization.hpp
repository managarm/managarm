#pragma once

#include <hel.h>

#include <thor-internal/address-space.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/rcu-base.hpp>

namespace thor {
	struct VirtualizedCpu : RcuProtected {
		virtual frg::expected<Error, HelVmexitReason> run() = 0;

		virtual void storeRegs(const HelVirtualizationRegs *regs) = 0;
		virtual void loadRegs(HelVirtualizationRegs *res) = 0;

		virtual bool assertInterrupt(uint64_t number, bool level) = 0;

	protected:
		~VirtualizedCpu() = default;
	};

	struct VirtualizedPageSpace : VirtualSpace, RcuProtected {
		VirtualizedPageSpace(VirtualOperations *ops) : VirtualSpace{ops} {}

	protected:
		~VirtualizedPageSpace() = default;
	};
}
