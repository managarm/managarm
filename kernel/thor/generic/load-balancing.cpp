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

frg::eternal<LoadBalancer> loadBalancer;

} // namespace

THOR_DEFINE_PERCPU(lbNode);

smarter::shared_ptr<LbControlBlock> LbControlBlock::create() {
	auto ptr = allocate_rcu_shared<LbControlBlock>(*kernelAlloc, CtorToken{});
	ptr->self_ = ptr;
	return ptr;
}

void LbControlBlock::finalizeBeforeRcu() {
	// Spare control blocks are never linked.
	if (!node_)
		return;

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&node_->mutex);

	node_->tasks.erase(this);
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

	auto cb = LbControlBlock::create();
	cb->thread_ = thread->self.lock();
	cb->node_ = node;

	{
		auto irqLock = frg::guard(&irqMutex());
		auto stateLock = frg::guard(&thread->_lbState.mutex_);
		auto threadLock = frg::guard(&thread->_mutex);

		auto now = getClockNanos();
		{
			auto lock = frg::guard(&node->mutex);

			node->tasks.push_back(cb.get());
			auto nodeLoad = node->currentLoad(now);
			nodeLoad.runnable += thread->_averagesAt(nodeLoad.timestamp).runnable;
			if (thread->_isRunnable())
				++nodeLoad.numRunnable;
			node->load.store(nodeLoad);
		}
		thread->_lbState.cb_.store(cb.get(), std::memory_order_release);
		thread->_lbState.cbRef_ = std::move(cb);
	}
}

void LoadBalancer::disconnect(Thread *thread) {
	auto cb = std::move(thread->_lbState.cbRef_);
	if (!cb)
		return;
	auto *node = cb->node_;

	{
		auto irqLock = frg::guard(&irqMutex());
		auto threadLock = frg::guard(&thread->_mutex);

		// The thread is not runnable anymore; only its remaining average leaves the node.
		// Otherwise, the node would carry load that no thread in its tasks accounts for.
		auto now = getClockNanos();
		{
			auto lock = frg::guard(&node->mutex);

			// The sum is only exact up to rounding.
			auto nodeLoad = node->currentLoad(now);
			auto load = thread->_averagesAt(nodeLoad.timestamp).runnable;
			nodeLoad.runnable -= frg::min(nodeLoad.runnable, load);
			node->load.store(nodeLoad);
		}
	}
}

void LoadBalancer::setAffinity(Thread *thread, frg::span<const uint8_t> mask) {
	assert(mask.size() == LbThreadState::affinityMaskSize());
	assert(LbThreadState::findFirstCpu(mask) != static_cast<size_t>(-1));

	auto *state = &thread->_lbState;
	// Whether the thread has to move is only known under the lock.
	// Allocate the replacement control block up front instead of allocating there.
	auto newCb = LbControlBlock::create();
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&state->mutex_);

		state->setAffinityMask_(mask);
		auto *cb = state->cb_.load(std::memory_order_relaxed);
		if (!state->inAffinityMask_(cb->node_->cpu->cpuIndex)) {
			// Pick the least-loaded allowed CPU so that shrinking a mask
			// does not herd threads onto its first CPU.
			auto now = getClockNanos();
			size_t bestIndex = static_cast<size_t>(-1);
			uint64_t bestLoad = 0;
			for (size_t i = 0; i < getCpuCount(); ++i) {
				if (!state->inAffinityMask_(i))
					continue;
				auto load = lbNode.getFor(i).currentLoad(now).load();
				if (bestIndex == static_cast<size_t>(-1) || load < bestLoad) {
					bestIndex = i;
					bestLoad = load;
				}
			}
			doMigration_(thread, &lbNode.getFor(bestIndex), std::move(newCb));
		}
	}

	// Also raise the condition if only the mask changed:
	// the thread may still be on its way to the assigned CPU from an earlier migration.
	Thread::migrateOther(thread->self);
}

void LoadBalancer::updateRunnable(Thread *thread, uint64_t now, bool runnable) {
	// Threads are runnable before they are connected; connect() accounts for that.
	auto *cb = thread->_lbState.cb_.load(std::memory_order_relaxed);
	if (!cb)
		return;
	auto *node = cb->node_;

	// Unlike the other users of LbNode::mutex, this relies on the caller to disable IRQs.
	assert(!intsAreEnabled());
	auto lock = frg::guard(&node->mutex);

	auto nodeLoad = node->currentLoad(now);
	// If the sum is already past now, it lacks the change of the signal during [now, nodeLoad.timestamp].
	// By linearity, that contribution does not depend on the updates in between.
	uint64_t late = 0;
	if (nodeLoad.timestamp > now)
		late = advanceLoad(0, UINT64_C(1) << (loadShift + loadFractionShift),
				loadDecayFactor(nodeLoad.timestamp - now));
	if (runnable) {
		++nodeLoad.numRunnable;
		nodeLoad.runnable += late;
	} else {
		assert(nodeLoad.numRunnable);
		--nodeLoad.numRunnable;
		nodeLoad.runnable -= frg::min(nodeLoad.runnable, late);
	}
	node->load.store(nodeLoad);
}

void LoadBalancer::doMigration_(Thread *thread, LbNode *dstNode,
		smarter::shared_ptr<LbControlBlock> newCb) {
	auto *state = &thread->_lbState;
	auto *cb = state->cb_.load(std::memory_order_relaxed);
	auto *srcNode = cb->node_;
	assert(srcNode != dstNode);

	newCb->thread_ = cb->thread_;
	newCb->node_ = dstNode;

	// Dropping the last reference unlinks the old control block, which takes srcNode's mutex.
	// Hence, drop it after the locks below.
	smarter::shared_ptr<LbControlBlock> oldRef;
	{
		// The thread cannot change its run state (and thereby the load of its node) during the move.
		auto threadLock = frg::guard(&thread->_mutex);

		auto now = getClockNanos();
		bool runnable = thread->_isRunnable();

		// Link the replacement before unlinking the old control block.
		// The thread is counted on both nodes in between rather than on neither.
		{
			auto lock = frg::guard(&dstNode->mutex);

			dstNode->tasks.push_back(newCb.get());
			auto nodeLoad = dstNode->currentLoad(now);
			nodeLoad.runnable += thread->_averagesAt(nodeLoad.timestamp).runnable;
			if (runnable)
				++nodeLoad.numRunnable;
			dstNode->load.store(nodeLoad);
		}
		state->cb_.store(newCb.get(), std::memory_order_release);
		oldRef = std::exchange(state->cbRef_, std::move(newCb));
		{
			auto lock = frg::guard(&srcNode->mutex);

			// The sum is only exact up to rounding.
			auto nodeLoad = srcNode->currentLoad(now);
			auto load = thread->_averagesAt(nodeLoad.timestamp).runnable;
			nodeLoad.runnable -= frg::min(nodeLoad.runnable, load);
			if (runnable) {
				assert(nodeLoad.numRunnable);
				--nodeLoad.numRunnable;
			}
			srcNode->load.store(nodeLoad);
		}
	}
}

coroutine<void> LoadBalancer::run_(CpuData *cpu) {
	auto *thisNode = &lbNode.get(cpu);

	bool joined = false;

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

		// Sum load once, then publish it to all CPUs through the barrier.
		if (!cpu->cpuIndex) {
			auto now = getClockNanos();
			systemLoad_ = 0;
			for (size_t i = 0; i < getCpuCount(); ++i)
				systemLoad_ += lbNode.getFor(i).currentLoad(now).load();
		}
		co_await barrier_.async_wait(barrier_.arrive());

		auto systemLoad = systemLoad_;
		uint64_t idealLoad = systemLoad / getCpuCount();
		if (debugLb && cpu == getCpuData(0))
			infoLogger() << "Total system load is " << systemLoad
					<< " (ideal load: " << idealLoad << ")" << frg::endlog;

		if (enableLb) {
			// Distribute load from other CPUs to this CPU.
			// Start at the next CPU such that the CPUs do not visit the sources in the same order.
			auto numCpus = getCpuCount();
			for (size_t k = 1; k < numCpus; ++k) {
				auto sourceIndex = (cpu->cpuIndex + k) % numCpus;
				balanceBetween_(&lbNode.getFor(sourceIndex), thisNode, idealLoad);
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
		// The thread's load and the node's sum are rounded separately.
		if (stolenLoad >= srcLoad)
			return false;
		uint64_t srcLoadPostMove = srcLoad - stolenLoad;
		uint64_t dstLoadPostMove = dstLoad + stolenLoad;

		uint64_t maxLoad = frg::max(srcLoad, dstLoad);
		uint64_t maxLoadPostMove = frg::max(srcLoadPostMove, dstLoadPostMove);
		return maxLoadPostMove < maxLoad;
	};

	// Replacement control block for the next migration, allocated outside of all locks.
	smarter::shared_ptr<LbControlBlock> spare;
	{
		IplGuard<ipl::noSchedule> rcuGuard;

		auto now = getClockNanos();
		for (auto *cb : srcNode->tasks) {
			auto srcLoad = srcNode->currentLoad(now).load();
			auto dstLoad = dstNode->currentLoad(now).load();

			// Do not attempt to do load balancing if source and destination are both
			// undersubscribed. While it may still be possible to improve the balance,
			// it is probably not worth it in terms of effort and cache degradation.
			if (srcLoad < idealLoad && dstLoad < idealLoad)
				break;

			// Pulling from a less loaded CPU can never improve the balance.
			if (srcLoad <= dstLoad)
				break;

			// Do not pull beyond the ideal load, other CPUs would have to pull the excess again.
			if (dstLoad >= idealLoad)
				break;

			auto thread = cb->thread_.lock();
			if (!thread)
				continue;
			auto *state = &thread->_lbState;

			// Do not move threads with tiny contributions to the total load.
			auto load = thread->load().at(now).runnable;
			if (!load)
				continue;

			if (!improvesBalance(srcLoad, dstLoad, load))
				continue;

			if (!spare)
				spare = LbControlBlock::create();

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

					doMigration_(thread.get(), dstNode, std::move(spare));
					moved = true;
				}
			}
			if (moved) {
				// Notify the thread such that it eventually moves to its assigned CPU.
				Thread::migrateOther(thread);
			}
		}
	}
}

} // namespace thor
