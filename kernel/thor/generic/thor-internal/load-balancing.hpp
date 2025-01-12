#pragma once

#include <async/barrier.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/thread.hpp>

namespace thor {

struct LbNode;

// Per-thread control block that is allocated by the load balancer.
struct LbControlBlock {
	friend struct LbNode;
	friend struct LoadBalancer;

	LbControlBlock(Thread *thread, LbNode *node)
	: thread_{thread->self.lock()}, node_{node} {}

	CpuData *getAssignedCpu() {
		return _assignedCpu.load(std::memory_order_relaxed);
	}

private:
	// Immutable.
	smarter::weak_ptr<Thread> thread_;

	// CPU that the corresponding thread *should* run on.
	// Not necessarily the CPU that the thread runs on currently.
	std::atomic<CpuData *> _assignedCpu{nullptr};

	// Protected by the LbNode that currently owns the node.
	LbNode *node_{nullptr};

	// Protected by the LbNode that currently owns the node.
	frg::default_list_hook<LbControlBlock> hook_;

	// Load of the thread associated with this control block.
	// Protected by the LbNode that currently owns the node.
	uint64_t load_{0};
};

// Per-CPU load balancing data structure.
struct LbNode {
	CpuData *cpu{nullptr};

	frg::ticket_spinlock mutex;

	// Protected by mutex.
	frg::intrusive_list<
		LbControlBlock,
		frg::locate_member<
			LbControlBlock,
			frg::default_list_hook<LbControlBlock>,
			&LbControlBlock::hook_
		>
	> tasks;

	// Constant during main phase of load balancing.
	uint64_t totalLoad{0};

	// Equal to totalLoad before load balancing but updated during load balancing.
	// Protected by mutex during main phase of load balancing.
	uint64_t currentLoad{0};
};

extern PerCpu<LbNode> lbNode;

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

private:
	coroutine<void> run_(CpuData *cpu);

	// Move tasks from srcNode to dstNode to balance load.
	// newLoad: newLoad at dstNode after balancing.
	void balanceBetween_(LbNode *srcNode, LbNode *dstNode, uint64_t &newLoad, uint64_t idealLoad);

	async::barrier barrier_;
};

} // namespace thor
