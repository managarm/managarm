#include <frg/unique.hpp>
#include <thor-internal/load-balancing.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

namespace {

constexpr bool debugLb = false;

// Basic settings.
constexpr bool enableLb = true;
constexpr uint64_t lbInterval = 100'000'000;

// Load decay factor (scale is hardcoded to 8 below) and decay interval.
constexpr uint64_t lbDecay = 184;
constexpr uint64_t lbDecayInterval = 1'000'000'000;

frg::eternal<LoadBalancer> loadBalancer;

} // namespace

THOR_DEFINE_PERCPU(lbNode);

LoadBalancer &LoadBalancer::singleton() {
	return loadBalancer.get();
}

LoadBalancer::LoadBalancer()
: barrier_{0} {}

void LoadBalancer::setOnline(CpuData *cpu) {
	auto *node = &lbNode.get(cpu);
	node->cpu = cpu;
	async::detach_with_allocator(*kernelAlloc, loadBalancer->run_(cpu));
}

void LoadBalancer::connect(Thread *thread, CpuData *cpu) {
	assert(!thread->_lbCb);
	auto *node = &lbNode.get(cpu);

	auto cb = frg::construct<LbControlBlock>(*kernelAlloc, thread, node);
	cb->_assignedCpu.store(cpu, std::memory_order_relaxed);
	thread->_lbCb = cb;

	// The LbControlBlock is now owned by the node.
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&node->mutex);

		node->tasks.push_back(cb);
	}
}

coroutine<void> LoadBalancer::run_(CpuData *cpu) {
	auto *thisNode = &lbNode.get(cpu);

	bool joined = false;
	uint64_t lastDecay = systemClockSource()->currentNanos();

	while(true) {
		// Global barrier to wait for initiation of load balancing.
		async::barrier::arrival_token token;
		if (!joined) {
			token = barrier_.arrive_and_join();
			joined = true;
		} else {
			token = barrier_.arrive();
		}
		co_await barrier_.async_wait(token);

		if (debugLb)
			infoLogger() << "CPU #" << cpu->cpuIndex << " enters load balancing" << frg::endlog;

		bool applyDecay = false;
		auto now = systemClockSource()->currentNanos();
		if (now - lastDecay >= lbDecayInterval) {
			applyDecay = true;
			lastDecay = now;
		}

		// On this CPU, estimate the load.
		uint64_t load = 0;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&thisNode->mutex);

			auto it = thisNode->tasks.begin();
			while (it != thisNode->tasks.end()) {
				auto currentIt = it;
				auto *cb = *currentIt;
				++it;

				// cb is owned by thisNode.
				// We deallocate the control block once the thread has been destroyed.
				auto thread = cb->thread_.lock();
				if (!thread) {
					thisNode->tasks.erase(currentIt);
					frg::destruct(*kernelAlloc, cb);
					continue;
				}

				thread->updateLoad();
				if (applyDecay)
					thread->decayLoad(lbDecay, 8);
				cb->load_ = thread->loadLevel();
				load += cb->load_;
			}
		}

		thisNode->totalLoad = load;
		thisNode->currentLoad = load;

		if (debugLb)
			infoLogger() << "CPU #" << cpu->cpuIndex << " has load " << load << frg::endlog;

		// Global barrier to wait until all CPUs know their load level.
		co_await barrier_.async_wait(barrier_.arrive());

		// Enter our own WorkQueue such that all CPUs can balance in parallel.
		co_await cpu->generalWorkQueue->enter();

		// Sum load of all CPUs.
		// TODO: Doing this on all CPUs is unnecessary. However, it is also reasonably fast
		//       and might be preferable over synchronization overhead.
		uint64_t systemLoad = 0;
		for (size_t i = 0; i < getCpuCount(); ++i)
			systemLoad += lbNode.getFor(i).totalLoad;
		uint64_t idealLoad = systemLoad / getCpuCount();
		if (debugLb && cpu == getCpuData(0))
			infoLogger() << "Total system load is " << systemLoad
					<< " (ideal load: " << idealLoad << ")" << frg::endlog;

		if (enableLb) {
			// Distribute load from other CPUs to this CPU.
			// TODO: This loop probably does not scale very well since all CPUs try to pull from
			//       all other CPUs in the same order (and this can cause lock contention).
			uint64_t newLoad = thisNode->totalLoad;
			for (size_t i = 0; i < getCpuCount(); ++i) {
				auto *toCpu = getCpuData(i);
				if (cpu != toCpu)
					balanceBetween_(&lbNode.get(toCpu), thisNode, newLoad, idealLoad);
			}
		}

		// Balance load again after some time has passed.
		// Note that we only wait on CPU zero. All other CPUs wait on the barrier instead.
		if (!cpu->cpuIndex)
			co_await generalTimerEngine()->sleep(systemClockSource()->currentNanos() + lbInterval);
	}

	co_return;
}

void LoadBalancer::balanceBetween_(LbNode *srcNode, LbNode *dstNode, uint64_t &newLoad, uint64_t idealLoad) {
	auto improvesBalance = [] (uint64_t srcLoad, uint64_t dstLoad, uint64_t stolenLoad) -> bool {
		uint64_t srcLoadPostMove = srcLoad - stolenLoad;
		uint64_t dstLoadPostMove = dstLoad + stolenLoad;

		uint64_t maxLoad = frg::max(srcLoad, dstLoad);
		uint64_t maxLoadPostMove = frg::max(srcLoadPostMove, dstLoadPostMove);
		return maxLoadPostMove < maxLoad;
	};

	// Remove tasks from srcNode, put them into a temporary list.
	frg::intrusive_list<
		LbControlBlock,
		frg::locate_member<
			LbControlBlock,
			frg::default_list_hook<LbControlBlock>,
			&LbControlBlock::hook_
		>
	> stolenTasks;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&srcNode->mutex);

		auto it = srcNode->tasks.begin();
		while (it != srcNode->tasks.end()) {
			auto currentIt = it;
			auto *cb = *currentIt;
			++it;

			// Do not attempt to do load balancing if source and destination are both
			// undersubscribed. While it may still be possible to improve the balance,
			// it is probably not worth it in terms of effort and cache degradation.
			if (srcNode->currentLoad < idealLoad && newLoad < idealLoad)
				break;

			// Do not move threads with tiny contributions to the total load.
			if (!cb->load_)
				continue;

			if (!improvesBalance(srcNode->currentLoad, newLoad, cb->load_))
				continue;

			if (debugLb)
				infoLogger() << "Moving thread with load " << cb->load_
						<< " from CPU " << srcNode->cpu->cpuIndex
						<< " to CPU " << dstNode->cpu->cpuIndex << frg::endlog;

			// Move ownership from srcNode to dstNode.
			assert(cb->node_ == srcNode);
			srcNode->tasks.erase(currentIt);
			cb->node_ = dstNode;
			cb->_assignedCpu.store(dstNode->cpu, std::memory_order_relaxed);
			stolenTasks.push_back(cb);

			srcNode->currentLoad -= cb->load_;
			newLoad += cb->load_;
		}
	}

	// Add tasks from temporary list to dstNode.
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&dstNode->mutex);

		dstNode->tasks.splice(dstNode->tasks.end(), stolenTasks);
	}
}

} // namespace thor
