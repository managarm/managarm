#pragma once

#include <assert.h>
#include <frg/slab.hpp>
#include <frg/spinlock.hpp>
#include <frg/manual_box.hpp>
#include <physical-buddy.hpp>
#include <thor-internal/arch/stack.hpp>

namespace thor {

struct IrqSpinlock {
	constexpr IrqSpinlock() = default;

	void lock();
	void unlock();

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

class KernelVirtualAlloc {
public:
	KernelVirtualAlloc();

	uintptr_t map(size_t length);
	void unmap(uintptr_t address, size_t length);

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

using KernelAlloc = frg::slab_allocator<KernelVirtualAlloc, IrqSpinlock>;

extern constinit frg::manual_box<KernelVirtualAlloc> kernelVirtualAlloc;

extern constinit frg::manual_box<
	frg::slab_pool<
		KernelVirtualAlloc,
		IrqSpinlock
	>
> kernelHeap;

extern constinit frg::manual_box<KernelAlloc> kernelAlloc;

struct Allocator {
	void *allocate(size_t size) {
		return kernelAlloc->allocate(size);
	}

	void deallocate(void *p, size_t size) {
		kernelAlloc->deallocate(p, size);
	}
};

} // namespace thor
