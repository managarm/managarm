#include <stdlib.h>
#include <bit>
#include <future>
#include <iostream>
#include <thread>

#include <hel.h>
#include <hel-syscalls.h>
#include <helix/dispatcher-pool.hpp>

namespace helix {

namespace {

// Number of CPUs that we have access to.
size_t queryCpuCount() {
	uint8_t mask[64];
	size_t actualSize;
	HEL_CHECK(helGetAffinity(kHelThisThread, mask, sizeof(mask), &actualSize));

	size_t count = 0;
	for(size_t i = 0; i < actualSize; ++i)
		count += std::popcount(mask[i]);
	return count;
}

void workerMain(std::promise<async::run_queue *> promise) {
	promise.set_value(Dispatcher::global().runQueue());
	async::run_forever(currentDispatcher);
	abort(); // We should never get here.
}

} // anonymous namespace

DispatcherPool &DispatcherPool::global() {
	static DispatcherPool singleton;
	return singleton;
}

DispatcherPool::DispatcherPool() {
	ownerContext_ = async::current_run_queue_context();

	// Adopt the owner as the first member of this pool.
	members_.push_back(Dispatcher::global().runQueue());
}

void DispatcherPool::enter_() {
	assert(!inBlockOn_ && "blockOn() must not be nested");
	assert(async::current_run_queue_context() == ownerContext_
			&& "blockOn() must be called from the owner thread");

	if(live_.load(std::memory_order_relaxed))
		return;

	auto numThreads = threadCount_ ? threadCount_ : queryCpuCount();
	std::cout << "helix: Running dispatcher pool on " << numThreads
			<< " threads" << std::endl;

	assert(members_.size() == 1); // The owner is the first member.
	for(size_t i = 1; i < numThreads; ++i) {
		std::promise<async::run_queue *> promise;
		auto future = promise.get_future();

		// The workers are never joined for now to prevent UAF
		// when there are still coroutines pointing to the helix::Dispatchers.
		std::thread{workerMain, std::move(promise)}.detach();

		members_.push_back(future.get());
	}

	live_.store(true, std::memory_order_release);
}

} // namespace helix
