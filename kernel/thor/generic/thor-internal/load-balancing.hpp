#pragma once

#include <async/barrier.hpp>
#include <frg/array.hpp>
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

	LbControlBlock(Thread *thread)
	: thread_{thread->self.lock()}, affinityMask_{*kernelAlloc} {
		affinityMask_.resize(affinityMaskSize());
		for (size_t i = 0; i < getCpuCount(); ++i)
			affinityMask_[i / 8] |= (1 << (i % 8));
	}

	CpuData *getAssignedCpu() {
		return _assignedCpu.load(std::memory_order_acquire);
	}

	void setAssignedCpu(CpuData *cpu) {
		_assignedCpu.store(cpu, std::memory_order_release);
	}

	LbNode *getNode() {
		return node_.load(std::memory_order_acquire);
	}

	// Precondition: mask.size() == affinityMaskSize().
	void getAffinityMask(frg::span<uint8_t> mask) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		getAffinityMaskLocked(mask);
	}

	// Precondition: mask.size() == affinityMaskSize().
	// Precondition: at least one bit of mask is set.
	void setAffinityMask(frg::span<const uint8_t> mask) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		setAffinityMaskLocked(mask);
	}

	bool inAffinityMask(size_t cpuIndex) {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		return inAffinityMaskLocked(cpuIndex);
	}

	static size_t findFirstCpu(frg::span<const uint8_t> mask) {
		for(size_t i = 0; i < getCpuCount(); ++i)
			if(mask[i / 8] & (1 << (i % 8)))
				return i;
		return static_cast<size_t>(-1);
	}

private:
	// Immutable.
	smarter::weak_ptr<Thread> thread_;

	// CPU that the corresponding thread *should* run on.
	// Not necessarily the CPU that the thread runs on currently.
	std::atomic<CpuData *> _assignedCpu{nullptr};

	// Advisory ownership snapshot. List membership under the expected node's
	// mutex is authoritative.
	std::atomic<LbNode *> node_{nullptr};

	// Protected by the LbNode that currently owns the node.
	frg::default_list_hook<LbControlBlock> hook_;

	// Load of the thread associated with this control block.
	// Protected by the LbNode that currently owns the node.
	uint64_t load_{0};

	// Serializes explicit affinity setters for this control block.
	frg::ticket_spinlock affinitySetterMutex_;

	// Set while an explicit setter owns affinitySetterMutex_. Automatic
	// balancing rejects active control blocks instead of waiting on them.
	std::atomic<bool> affinityUpdateActive_{false};

	// Protects affinityMask_. Ownership transactions acquire this after their
	// node locks. It does not protect assignedCpu_, node_, hook_ or load_.
	frg::ticket_spinlock mutex_;

	// Protected by mutex_.
	frg::vector<uint8_t, KernelAlloc> affinityMask_;

	void getAffinityMaskLocked(frg::span<uint8_t> mask) {
		assert(affinityMask_.size() == mask.size());
		memcpy(mask.data(), affinityMask_.data(), affinityMaskSize());
	}

	void setAffinityMaskLocked(frg::span<const uint8_t> mask) {
		assert(affinityMask_.size() == mask.size());
		assert(findFirstCpu(mask) != static_cast<size_t>(-1));
		memcpy(affinityMask_.data(), mask.data(), affinityMaskSize());
	}

	bool inAffinityMaskLocked(size_t cpuIndex) {
		return affinityMask_[cpuIndex / 8] & (1 << (cpuIndex % 8));
	}
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

	// Protected accounting snapshot used to derive ideal load.
	uint64_t totalLoad{0};

	// Live list ownership accounting. Protected by mutex.
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

	// Synchronously commit affinity metadata and ownership, then request an
	// asynchronous migration at the next return-to-user condition point.
	void setAffinity(Thread *thread, frg::span<const uint8_t> mask);

private:
	coroutine<void> run_(CpuData *cpu);

	// Move a bounded batch of tasks from srcNode to dstNode to balance load.
	void balanceBetween_(LbNode *srcNode, LbNode *dstNode, uint64_t idealLoad);

	async::barrier barrier_;
	uint64_t systemLoad_{0};
};

} // namespace thor
