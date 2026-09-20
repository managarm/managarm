#include <frg/unique.hpp>
#include <thor-internal/load-balancing.hpp>
#include <thor-internal/rcu.hpp>
#include <thor-internal/thread.hpp>
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

void LbControlBlock::retire_(RcuCallable *base) {
	frg::destruct(*kernelAlloc, static_cast<LbControlBlock *>(base));
}

LoadBalancer &LoadBalancer::singleton() {
	return loadBalancer.get();
}

LoadBalancer::LoadBalancer()
: barrier_{0} {}

void LoadBalancer::setOnline(CpuData *cpu) {
	auto *node = &lbNode.get(cpu);
	node->cpu = cpu;
	spawnOnWorkQueue(*kernelAlloc, cpu->generalWorkQueue, loadBalancer->run_(cpu));
}

void LoadBalancer::connect(Thread *thread, CpuData *cpu) {
	assert(!thread->_lbState.cb_.load(std::memory_order_relaxed));
	auto *node = &lbNode.get(cpu);

	auto cb = frg::construct<LbControlBlock>(*kernelAlloc, thread->self.lock());
	cb->node_ = node;

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&node->mutex);

		node->tasks.push_back(cb);
	}
	thread->_lbState.cb_.store(cb, std::memory_order_release);
}

void LoadBalancer::doMigration_(LbThreadState *state, LbNode *dstNode, LbControlBlock *newCb) {
	auto *cb = state->cb_.load(std::memory_order_relaxed);
	auto *srcNode = cb->node_;
	assert(srcNode != dstNode);
	auto load = cb->load_.load(std::memory_order_relaxed);

	// Link the replacement before unlinking the old control block.
	// The thread is counted on both nodes in between rather than on neither.
	newCb->thread_ = cb->thread_;
	newCb->node_ = dstNode;
	newCb->load_.store(load, std::memory_order_relaxed);
	{
		auto lock = frg::guard(&dstNode->mutex);

		dstNode->tasks.push_back(newCb);
		dstNode->currentLoad.fetch_add(load, std::memory_order_relaxed);
	}
	state->cb_.store(newCb, std::memory_order_release);
	{
		auto lock = frg::guard(&srcNode->mutex);

		assert(!cb->unlinked_);
		srcNode->tasks.erase(cb);
		cb->unlinked_ = true;
		// A concurrent accounting pass may have published a sum that already
		// excludes the control block.
		auto current = srcNode->currentLoad.load(std::memory_order_relaxed);
		srcNode->currentLoad.store(current - frg::min(current, load), std::memory_order_relaxed);
	}
	submitRcu(cb, &LbControlBlock::retire_);
}

coroutine<void> LoadBalancer::run_(CpuData *cpu) {
	auto *thisNode = &lbNode.get(cpu);

	bool joined = false;
	uint64_t lastDecay = getClockNanos();

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
		auto now = getClockNanos();
		if (now - lastDecay >= lbDecayInterval) {
			applyDecay = true;
			lastDecay = now;
		}

		// On this CPU, estimate the load.
		uint64_t load = 0;
		{
			IplGuard<ipl::noSchedule> rcuGuard;

			auto it = thisNode->tasks.begin();
			while (it != thisNode->tasks.end()) {
				auto *cb = *it;
				++it;

				// Control blocks of destroyed threads are unlinked here
				// (unless a migration already did so) and deallocated after the grace period.
				auto thread = cb->thread_.lock();
				if (!thread) {
					bool unlink;
					{
						auto irqLock = frg::guard(&irqMutex());
						auto lock = frg::guard(&thisNode->mutex);

						unlink = !cb->unlinked_;
						if (unlink) {
							thisNode->tasks.erase(cb);
							cb->unlinked_ = true;
						}
					}
					if (unlink)
						submitRcu(cb, &LbControlBlock::retire_);
					continue;
				}

				thread->updateLoad(applyDecay, lbDecay, 8);
				auto threadLoad = thread->loadLevel();
				cb->load_.store(threadLoad, std::memory_order_relaxed);
				load += threadLoad;
			}
		}

		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&thisNode->mutex);

			thisNode->totalLoad = load;
			thisNode->currentLoad.store(load, std::memory_order_relaxed);
		}

		if (debugLb)
			infoLogger() << "CPU #" << cpu->cpuIndex << " has load " << load << frg::endlog;

		// Global barrier to wait until all CPUs know their load level.
		co_await barrier_.async_wait(barrier_.arrive());

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
			for (size_t i = 0; i < getCpuCount(); ++i) {
				auto *toCpu = getCpuData(i);
				if (cpu != toCpu)
					balanceBetween_(&lbNode.get(toCpu), thisNode, idealLoad);
			}
		}

		// Balance load again after some time has passed.
		// Note that we only wait on CPU zero. All other CPUs wait on the barrier instead.
		if (!cpu->cpuIndex)
			co_await generalTimerEngine()->sleep(getClockNanos() + lbInterval);
	}

	co_return;
}

void LoadBalancer::balanceBetween_(LbNode *srcNode, LbNode *dstNode, uint64_t idealLoad) {
	auto improvesBalance = [] (uint64_t srcLoad, uint64_t dstLoad, uint64_t stolenLoad) -> bool {
		uint64_t srcLoadPostMove = srcLoad - stolenLoad;
		uint64_t dstLoadPostMove = dstLoad + stolenLoad;

		uint64_t maxLoad = frg::max(srcLoad, dstLoad);
		uint64_t maxLoadPostMove = frg::max(srcLoadPostMove, dstLoadPostMove);
		return maxLoadPostMove < maxLoad;
	};

	// Replacement control block for the next migration, allocated outside of all locks.
	LbControlBlock *spare = nullptr;
	{
		IplGuard<ipl::noSchedule> rcuGuard;

		for (auto *cb : srcNode->tasks) {
			auto srcLoad = srcNode->currentLoad.load(std::memory_order_relaxed);
			auto dstLoad = dstNode->currentLoad.load(std::memory_order_relaxed);

			// Do not attempt to do load balancing if source and destination are both
			// undersubscribed. While it may still be possible to improve the balance,
			// it is probably not worth it in terms of effort and cache degradation.
			if (srcLoad < idealLoad && dstLoad < idealLoad)
				break;

			// Do not move threads with tiny contributions to the total load.
			auto load = cb->load_.load(std::memory_order_relaxed);
			if (!load)
				continue;

			if (!improvesBalance(srcLoad, dstLoad, load))
				continue;

			auto thread = cb->thread_.lock();
			if (!thread)
				continue;
			auto *state = &thread->_lbState;

			if (!spare)
				spare = frg::construct<LbControlBlock>(*kernelAlloc);

			bool moved = false;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&state->mutex_);

				// Skip control blocks that a concurrent migration already replaced.
				if (state->cb_.load(std::memory_order_relaxed) == cb
						&& state->inAffinityMask_(dstNode->cpu->cpuIndex)) {
					if (debugLb)
						infoLogger() << "Moving thread with load " << load
								<< " from CPU " << srcNode->cpu->cpuIndex
								<< " to CPU " << dstNode->cpu->cpuIndex << frg::endlog;

					doMigration_(state, dstNode, spare);
					moved = true;
				}
			}
			if (moved) {
				spare = nullptr;
				// Notify the thread such that it eventually moves to its assigned CPU.
				Thread::migrateOther(thread);
			}
		}
	}
	if (spare)
		frg::destruct(*kernelAlloc, spare);
}

} // namespace thor
