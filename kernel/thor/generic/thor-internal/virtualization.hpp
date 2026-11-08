#pragma once

#include <hel.h>

#include <async/recurring-event.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/rcu-base.hpp>
#include <thor-internal/arch-generic/ints.hpp>

#include <atomic>

namespace thor {
	struct VirtualizedCpu : RcuProtected {
		// run() and the register accessors must not run concurrently, their callers must hold this guard.
		//
		// The guard construction fails if the vCPU is in use (or if vcpu is null), the user must
		// use the bool operator to see whether the guard was acquired successfully.
		struct UseGuard {
			explicit UseGuard(VirtualizedCpu *vcpu) {
				if(vcpu && !vcpu->inUse_.exchange(true, std::memory_order_acquire))
					vcpu_ = vcpu;
			}

			UseGuard(const UseGuard &) = delete;
			UseGuard &operator=(const UseGuard &) = delete;

			~UseGuard() {
				if(vcpu_)
					vcpu_->inUse_.store(false, std::memory_order_release);
			}

			explicit operator bool() const {
				return vcpu_;
			}

		private:
			VirtualizedCpu *vcpu_{nullptr};
		};

		// Runs the vCPU until it exits to userspace. completion completes the previous exit
		// if it needs one (see hel.h) and must be null otherwise.
		virtual frg::expected<Error, HelVmexitReason> run(const HelVmexitCompletion *completion) = 0;

		virtual void storeRegs(const HelVirtualizationRegs *regs) = 0;
		virtual void loadRegs(HelVirtualizationRegs *res) = 0;

		virtual bool assertInterrupt(uint64_t number, bool level) = 0;

		void kick() {
			kicked_.store(true, std::memory_order_seq_cst);
			notify();
		}

		// Makes a running vCPU exit (run() then continues) and wakes a waiting one, so that it
		// reevaluates its state (e.g., after an interrupt became pending).
		void notify() {
			if(auto cpu = runningOn_.load(std::memory_order_seq_cst); cpu)
				sendPingIpi(cpu);
			wakeEvent_.raise();
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

		// Consumes a pending kick (for vCPUs that wait outside of the guest).
		bool consumeKick() {
			return kicked_.exchange(false, std::memory_order_seq_cst);
		}

		bool kickPending() {
			return kicked_.load(std::memory_order_seq_cst);
		}

		// Raised by notify().
		async::recurring_event wakeEvent_;

	private:
		std::atomic_bool inUse_{false};
		std::atomic_bool kicked_{false};
		std::atomic<CpuData *> runningOn_{nullptr};
	};

	struct VirtualizedPageSpace : VirtualSpace, RcuProtected {
		VirtualizedPageSpace(VirtualOperations *ops) : VirtualSpace{ops} {}

	protected:
		~VirtualizedPageSpace() = default;
	};
}
