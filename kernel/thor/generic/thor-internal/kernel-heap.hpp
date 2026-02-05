#pragma once

#include <assert.h>
#include <frg/sharded_slab.hpp>
#include <frg/slab.hpp>
#include <frg/spinlock.hpp>
#include <frg/manual_box.hpp>
#include <physical-buddy.hpp>
#include <thor-internal/arch/stack.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/ipl.hpp>

namespace thor {

struct IrqSpinlock {
	constexpr IrqSpinlock() = default;

	void lock() {
		irqMutex().lock();
		_spinlock.lock();
	}

	void unlock() {
		_spinlock.unlock();
		irqMutex().unlock();
	}

private:
	frg::ticket_spinlock _spinlock;
};

struct KernelVirtualMemory {
	using Mutex = frg::ticket_spinlock;
public:
	static KernelVirtualMemory &global();

	// TODO: make this private
	KernelVirtualMemory();

	KernelVirtualMemory(const KernelVirtualMemory &other) = delete;
	
	KernelVirtualMemory &operator= (const KernelVirtualMemory &other) = delete;

	void *allocate(size_t length);
	void deallocate(void *pointer, size_t length);

private:
	Mutex mutex_;
};

class HeapSlabPolicy {
public:
	void *map(size_t length);
	void unmap(void *ptr, size_t length);

	bool enable_trace() {
#ifdef KERNEL_LOG_ALLOCATIONS
		return true;
#else
		return false;
#endif // KERNEL_LOG_ALLOCATIONS
	}

	template <typename F>
	void walk_stack(F functor) {
		walkThisStack(functor);
	}

	void unpoison(void *pointer, size_t size);
	void unpoison_expand(void *pointer, size_t size);
	void poison(void *pointer, size_t size);

	void output_trace(void *buffer, size_t size);
};

static_assert(frg::slab::has_poisoning_support<HeapSlabPolicy>);
#ifdef KERNEL_LOG_ALLOCATIONS
static_assert(frg::slab::has_trace_support<HeapSlabPolicy>);
#endif

extern PerCpu<frg::sharded_slab_pool<HeapSlabPolicy>> heapSlabPool;
// We use this variable to check for reentrancy (i.e., for error checking).
extern PerCpu<std::atomic<bool>> inSlabPool;

struct Allocator {
	struct Guard : IplGuard<ipl::schedule> {
		Guard() {
			auto cpu = getCpuData();
			assert(!inSlabPool.get(cpu).load(std::memory_order_relaxed));
			inSlabPool.get(cpu).store(true, std::memory_order_relaxed);
		}

		~Guard() {
			auto cpu = getCpuData();
			assert(inSlabPool.get(cpu).load(std::memory_order_relaxed));
			inSlabPool.get(cpu).store(false, std::memory_order_relaxed);
		}
	};

	void *allocate(size_t size) const {
		Guard guard;
		auto &pool = heapSlabPool.get();
		return pool.allocate(size);
	}

	void deallocate(void *p, size_t size) const {
		(void)size;
		Guard guard;
		auto &pool = heapSlabPool.get();
		pool.deallocate(p);
	}

	void free(void *p) const {
		Guard guard;
		auto &pool = heapSlabPool.get();
		pool.deallocate(p);
	}
};

// TODO: KernelAlloc the kernelAlloc global should be removed in favor of the Allocator class.
using KernelAlloc = Allocator;

extern constinit frg::manual_box<KernelAlloc> kernelAlloc;

} // namespace thor
