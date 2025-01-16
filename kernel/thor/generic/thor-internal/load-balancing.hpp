#pragma once

#include <async/barrier.hpp>
#include <frg/span.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/thread.hpp>

namespace thor {

struct LbNode;

// Per-thread control block that is allocated by the load balancer.
struct LbControlBlock {
	friend struct LbNode;
	friend struct LoadBalancer;

	static size_t affinityMaskSize() {
		return (getCpuCount() + 7) / 8;
	}

	LbControlBlock(Thread *thread, LbNode *node)
	: thread_{thread->self.lock()}, node_{node}, affinityMask_{*kernelAlloc} {
		affinityMask_.resize(affinityMaskSize());
		for (size_t i = 0; i < getCpuCount(); ++i)
			affinityMask_[i / 8] |= (1 << (i % 8));
	}

	CpuData *getAssignedCpu() {
		return _assignedCpu.load(std::memory_order_relaxed);
	}

	// Precondition: mask.size() == affinityMaskSize().
	void getAffinityMask(frg::span<uint8_t> mask) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		assert(affinityMask_.size() == mask.size());
		memcpy(mask.data(), affinityMask_.data(), affinityMaskSize());
	}

	// Precondition: mask.size() == affinityMaskSize().
	// Precondition: at least one bit of mask is set.
	void setAffinityMask(frg::span<const uint8_t> mask) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		assert(affinityMask_.size() == mask.size());
		assert(std::any_of(mask.begin(), mask.end(), [] (uint8_t x) -> bool { return x; }));
		memcpy(affinityMask_.data(), mask.data(), affinityMaskSize());
	}

	bool inAffinityMask(size_t cpuIndex) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		return affinityMask_[cpuIndex / 8] & (1 << (cpuIndex % 8));
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

	// Protects members that can be written by public functions.
	// Note that this mutex does not protect assigendCpu_, node_, hook_, load_ etc.
	frg::ticket_spinlock mutex_;

	// Protected by mutex_;
	frg::vector<uint8_t, KernelAlloc> affinityMask_;
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
