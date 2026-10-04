#pragma once

#include <string.h>

#include <async/barrier.hpp>
#include <frg/list.hpp>
#include <frg/seqlock.hpp>
#include <frg/span.hpp>
#include <frg/spinlock.hpp>
#include <frg/vector.hpp>
#include <smarter.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/load-tracking.hpp>
#include <thor-internal/rcu-base.hpp>

namespace thor {

struct LbControlBlock;
struct LbNode;
struct Thread;

// Per-thread load balancer state, embedded into Thread.
// The load balancer only reaches it through a strong reference to the thread,
// except for disconnect(), which runs from the thread's destructor.
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
	// Lock order: mutex_, then the thread's mutex, then LbNode::mutex.
	frg::ticket_spinlock mutex_;

	// Current control block.
	// Writers hold both mutex_ and the thread's mutex; it is read under either of them or under RCU.
	std::atomic<LbControlBlock *> cb_{nullptr};
	// Reference to cb_ that keeps it linked into its node. Protected like cb_ but not read under RCU.
	// disconnect() takes it without locks since no other reference to the thread exists anymore.
	smarter::shared_ptr<LbControlBlock> cbRef_;

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

// Membership record of one thread on one node. It is bound to its node for its whole lifetime:
// migrating the thread links a new control block into the destination node and drops this one.
struct LbControlBlock : RcuProtected {
	friend struct LbNode;
	friend struct LbThreadState;
	friend struct LoadBalancer;

private:
	struct CtorToken {};

public:
	static smarter::shared_ptr<LbControlBlock> create();

	LbControlBlock(CtorToken) { }

	// Unlinks this control block from its node; RCU keeps it traversable until the grace period ends.
	void finalizeBeforeRcu();

private:
	smarter::weak_ptr<LbControlBlock> self_;

	// Set before the control block is linked into the node's list, immutable afterwards.
	smarter::weak_ptr<Thread> thread_;

	// Set before the control block is linked into the node's list, immutable afterwards.
	LbNode *node_{nullptr};

	// Protected against writes by LbNode::mutex, traversed under RCU.
	frg::intrusive_rcu_list_hook<LbControlBlock> listHook_;
};

// Per-CPU load balancing data structure.
struct LbNode {
	CpuData *cpu{nullptr};

	// Serializes writers of tasks and load.
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

	// Sum of the loads of the threads in tasks.
	// The sum is adjusted whenever one of the threads starts or stops being runnable and whenever a thread is added or moved.
	// Modified under mutex, read without it (e.g., for placement decisions).
	frg::seqlock_cell<NodeLoad> load;

	// The result can be ahead of now if updates from other CPUs overtook the caller.
	// Callers apply their changes as of the result's timestamp.
	NodeLoad currentLoad(uint64_t now) {
		return load.load().at(now);
	}
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
	void connect(Thread *thread, CpuData *cpu);

	// Detaches a thread from the load balancer. Must be called before the thread is destructed.
	void disconnect(Thread *thread);

	// Synchronously commit the affinity mask and the assignment,
	// then request an asynchronous migration of the thread.
	// Precondition: mask.size() == affinityMaskSize().
	// Precondition: at least one bit of mask is set.
	void setAffinity(Thread *thread, frg::span<const uint8_t> mask);

	// Must be called when a thread starts or stops being runnable, at time now.
	// Precondition: the thread's mutex is held and IRQs are disabled.
	void updateRunnable(Thread *thread, uint64_t now, bool runnable);

private:
	coroutine<void> run_(CpuData *cpu);

	// Calls fn(thread, cb) for the threads of a node until fn returns false.
	// cb is the thread's control block on the node and stays alive during the call.
	template<typename F>
	void forEachThread_(LbNode *node, F fn);

	// Move tasks from srcNode to dstNode to balance load.
	void balanceBetween_(LbNode *srcNode, LbNode *dstNode, uint64_t idealLoad);

	// Replace the current control block of the thread by newCb, linked into dstNode.
	// Precondition: the thread's LbThreadState::mutex_ is held but not its mutex (which this takes).
	void doMigration_(Thread *thread, LbNode *dstNode, smarter::shared_ptr<LbControlBlock> newCb);

	async::barrier barrier_;
	uint64_t systemLoad_{0};
};

} // namespace thor
