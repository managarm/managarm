#pragma once

#include <string.h>

#include <async/barrier.hpp>
#include <frg/list.hpp>
#include <frg/span.hpp>
#include <frg/spinlock.hpp>
#include <frg/vector.hpp>
#include <smarter.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/rcu-base.hpp>

namespace thor {

struct LbControlBlock;
struct LbNode;
struct Thread;

// Per-thread load balancer state, embedded into Thread.
// The load balancer only reaches it through a strong reference to the thread.
struct LbThreadState {
	friend struct LoadBalancer;

	static size_t affinityMaskSize() {
		return (getCpuCount() + 7) / 8;
	}

	static size_t findFirstCpu(frg::span<const uint8_t> mask) {
		for(size_t i = 0; i < getCpuCount(); ++i)
			if(mask[i / 8] & (1 << (i % 8)))
				return i;
		return static_cast<size_t>(-1);
	}

	LbThreadState()
	: affinityMask_{*kernelAlloc} {
		affinityMask_.resize(affinityMaskSize());
		for (size_t i = 0; i < getCpuCount(); ++i)
			affinityMask_[i / 8] |= (1 << (i % 8));
	}

	// CPU that the corresponding thread *should* run on.
	// Not necessarily the CPU that the thread runs on currently.
	CpuData *getAssignedCpu();

	// Precondition: mask.size() == affinityMaskSize().
	void getAffinityMask(frg::span<uint8_t> mask) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		assert(affinityMask_.size() == mask.size());
		memcpy(mask.data(), affinityMask_.data(), affinityMaskSize());
	}

private:
	frg::ticket_spinlock mutex_;

	// Current control block.
	// Protected against writes by mutex_, read under RCU.
	std::atomic<LbControlBlock *> cb_{nullptr};

	// Protected by mutex_.
	frg::vector<uint8_t, KernelAlloc> affinityMask_;

	// Precondition: mask.size() == affinityMaskSize().
	// Precondition: at least one bit of mask is set.
	// Callers must hold mutex_.
	void setAffinityMask_(frg::span<const uint8_t> mask) {
		assert(affinityMask_.size() == mask.size());
		assert(findFirstCpu(mask) != static_cast<size_t>(-1));
		memcpy(affinityMask_.data(), mask.data(), affinityMaskSize());
	}

	// Callers must hold mutex_.
	bool inAffinityMask_(size_t cpuIndex) {
		return affinityMask_[cpuIndex / 8] & (1 << (cpuIndex % 8));
	}
};

// Accounting record of one thread on one node. It is bound to its node for its whole
// life: migrating the thread links a new control block into the destination node and retires
// this one via RCU, so node task lists can be traversed without their mutex.
struct LbControlBlock : RcuCallable {
	friend struct LbNode;
	friend struct LbThreadState;
	friend struct LoadBalancer;

	LbControlBlock(smarter::weak_ptr<Thread> thread)
	: thread_{std::move(thread)} { }

	LbControlBlock() = default;

private:
	static void retire_(RcuCallable *base);

	// Set before the control block is linked into the node's list, immutable afterwards.
	smarter::weak_ptr<Thread> thread_;

	// Set before the control block is linked into the node's list, immutable afterwards.
	LbNode *node_{nullptr};

	// Protected against writes by LbNode::mutex, traversed under RCU.
	frg::intrusive_rcu_list_hook<LbControlBlock> listHook_;

	// Whether the LbNode has been unlinked. Set before its retired via RCU.
	// Protected by LbNode::mutex.
	bool unlinked_{false};

	// Load of the thread as of the last accounting pass of node_.
	std::atomic<uint64_t> load_{0};
};

// Per-CPU load balancing data structure.
struct LbNode {
	CpuData *cpu{nullptr};

	// Serializes writers of tasks and protects currentLoad.
	frg::ticket_spinlock mutex;

	// Protected against writes by mutex, traversed under RCU.
	frg::intrusive_rcu_list<
		LbControlBlock,
		frg::locate_member<
			LbControlBlock,
			frg::intrusive_rcu_list_hook<LbControlBlock>,
			&LbControlBlock::listHook_
		>
	> tasks;

	// Accounting snapshot used to derive the ideal load.
	// Written under mutex, also read cross-CPU without the mutex (e.g., for placement decisions).
	std::atomic<uint64_t> totalLoad{0};

	// Equal to totalLoad before load balancing but updated during load balancing.
	// Modified under mutex, read without it.
	std::atomic<uint64_t> currentLoad{0};
};

extern PerCpu<LbNode> lbNode;

inline CpuData *LbThreadState::getAssignedCpu() {
	IplGuard<ipl::noSchedule> rcuGuard;
	return cb_.load(std::memory_order_acquire)->node_->cpu;
}

struct LoadBalancer {
	static LoadBalancer &singleton();

	LoadBalancer();

	LoadBalancer(const LoadBalancer &) = delete;
	LoadBalancer &operator= (const LoadBalancer &) = delete;

	// Must be called on each CPU before threads can be moved to that CPU.
	void setOnline(CpuData *cpu);

	// Attaches a thread to the load balancer.
	// The load balancer keeps a weak reference to the thread.
	// The thread is detached from the load balancer when the weak reference goes out of scope.
	void connect(Thread *thread, CpuData *cpu);

	// Synchronously commit the affinity mask and the assignment,
	// then request an asynchronous migration of the thread.
	// Precondition: mask.size() == affinityMaskSize().
	// Precondition: at least one bit of mask is set.
	void setAffinity(Thread *thread, frg::span<const uint8_t> mask);

private:
	coroutine<void> run_(CpuData *cpu);

	// Move tasks from srcNode to dstNode to balance load.
	void balanceBetween_(LbNode *srcNode, LbNode *dstNode, uint64_t idealLoad);

	// Replace the current control block of state by newCb, linked into dstNode.
	// Precondition: state->mutex_ is held.
	void doMigration_(LbThreadState *state, LbNode *dstNode, LbControlBlock *newCb);

	async::barrier barrier_;
};

} // namespace thor
