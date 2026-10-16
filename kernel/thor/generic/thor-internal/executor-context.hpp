#pragma once

#include <thor-internal/physical.hpp>
#include <thor-internal/rcu-base.hpp>

namespace thor {

struct WorkQueue;

// Caches free physical pages that back the kernel's slab allocator.
// Filled in contexts that can potentially block and consumed from the slab allocator (which cannot block).
struct HeapMagazine {
	HeapMagazine() = default;

	HeapMagazine(const HeapMagazine &) = delete;

	HeapMagazine &operator= (const HeapMagazine &) = delete;

	~HeapMagazine();

	PageChain pages;
	// Number of pages that the last failed map() was missing.
	size_t wanted = 0;
};

// This class uniquely identifies a thread or fiber.
// RCU protected such that we can reference this from mutex implementations,
// WorkQueue or similar with worrying about lifetimes.
struct ExecutorContext : RcuCallable {
	static ExecutorContext *create();

	// Destroy this ExecutorContext after RCU grace period.
	static void retire(ExecutorContext *ctx);

	ExecutorContext();

	ExecutorContext(const ExecutorContext &) = delete;

	ExecutorContext &operator= (const ExecutorContext &) = delete;

	// Used by AdaptiveMutex to test if the context is still active (i.e., running on any CPU).
	std::atomic<bool> active{false};
	WorkQueue *exceptionalWq{nullptr};
	HeapMagazine heapMagazine;
};

} // namespace thor
