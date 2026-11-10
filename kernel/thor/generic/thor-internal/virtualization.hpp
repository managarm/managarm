#pragma once

#include <hel.h>

#include <thor-internal/address-space.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/rcu-base.hpp>
#include <thor-internal/arch-generic/ints.hpp>

#include <atomic>

namespace thor {
	struct VirtualizedCpu : RcuProtected {
		virtual frg::expected<Error, HelVmexitReason> run(const HelVmexitCompletion *completion) = 0;

		virtual void storeRegs(const HelVirtualizationRegs *regs) = 0;
		virtual void loadRegs(HelVirtualizationRegs *res) = 0;

		virtual bool assertInterrupt(uint64_t number, bool level) = 0;

		void kick() {
			kicked_.store(true, std::memory_order_seq_cst);
			// If the vCPU is running, the IPI makes it exit.
			if(auto cpu = runningOn_.load(std::memory_order_seq_cst); cpu)
				sendPingIpi(cpu);
		}

	protected:
		~VirtualizedCpu() = default;

		// Must be called before each guest entry.
		// Returns true if run() needs to return because of a kick.
		bool enterGuest(CpuData *cpu) {
			runningOn_.store(cpu, std::memory_order_seq_cst);
			if(kicked_.exchange(false, std::memory_order_seq_cst)) {
				runningOn_.store(nullptr, std::memory_order_relaxed);
				return true;
			}
			return false;
		}

		// Must be called after each guest exit.
		void leaveGuest() {
			runningOn_.store(nullptr, std::memory_order_relaxed);
		}

	private:
		std::atomic_bool kicked_{false};
		std::atomic<CpuData *> runningOn_{nullptr};
	};

	struct VirtualizedPageSpace : VirtualSpace, RcuProtected {
		VirtualizedPageSpace(VirtualOperations *ops) : VirtualSpace{ops} {}

	protected:
		~VirtualizedPageSpace() = default;
	};
}
