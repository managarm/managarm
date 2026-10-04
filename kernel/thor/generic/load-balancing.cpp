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
// Overloaded CPUs ask for pulls at most once per interval.
constexpr uint64_t lbInterval = 100'000'000;

// Differences in load below this margin are not worth acting on.
// This also applies to threads: moving a thread with less load does not change the balance.
constexpr uint64_t lbMargin = (UINT64_C(1) << loadShift) / 16;
// Number of CPUs that a CPU tries to pull from before it gives up.
constexpr size_t lbMaxSources = 4;

// A move must lower the larger of the two loads by at least a fraction of the moved load.
// This prevents threads from bouncing between CPUs due to small fluctuations in load.
bool improvesBalance(uint64_t srcLoad, uint64_t dstLoad, uint64_t movedLoad) {
	return dstLoad + movedLoad + movedLoad / 8 <= srcLoad;
}

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

void LoadBalancer::setOnline(CpuData *cpu) {
	auto *node = &lbNode.get(cpu);
	node->cpu = cpu;
	node->online.store(true, std::memory_order_release);
	spawnOnWorkQueue(*kernelAlloc, cpu->generalWorkQueue, loadBalancer->runRequestLoop_(cpu));
	spawnOnWorkQueue(*kernelAlloc, cpu->generalWorkQueue, loadBalancer->runPullLoop_(cpu));
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
	// If walkers hold references to the control block, the last of them unlinks it.
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

void LoadBalancer::checkOverload() {
	assert(!intsAreEnabled());
	if (!enableLb || getCpuCount() < 2)
		return;
	auto *node = &lbNode.get();

	// Threads only wait for this CPU if more than one of them is runnable.
	if (node->load.load().numRunnable < 2)
		return;

	auto now = getClockNanos();
	if (now < node->lastRequest.load(std::memory_order_relaxed) + lbInterval)
		return;
	node->lastRequest.store(now, std::memory_order_relaxed);

	// This is called from contexts that cannot walk the threads of this CPU.
	node->overloaded.store(true, std::memory_order_release);
	node->overloadEvent.raise();
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
	// Hence, drop it after the locks below (callers still hold mutex_, which precedes LbNode::mutex);
	// if walkers hold references, the last of them unlinks it.
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

coroutine<void> LoadBalancer::runRequestLoop_(CpuData *cpu) {
	auto *thisNode = &lbNode.get(cpu);

	while (true) {
		if (!thisNode->overloaded.exchange(false, std::memory_order_acq_rel)) {
			co_await thisNode->overloadEvent.async_wait_if([&] {
				return !thisNode->overloaded.load(std::memory_order_relaxed);
			});
			continue;
		}

		requestPull_(thisNode);
	}
}

coroutine<void> LoadBalancer::runPullLoop_(CpuData *cpu) {
	auto *thisNode = &lbNode.get(cpu);

	while (true) {
		auto hintIndex = thisNode->pullSource.exchange(noCpu, std::memory_order_acq_rel);
		if (hintIndex == noCpu) {
			co_await thisNode->pullEvent.async_wait_if([&] {
				return thisNode->pullSource.load(std::memory_order_relaxed) == noCpu;
			});
			continue;
		}

		pull_(thisNode, hintIndex);
	}
}

uint64_t LoadBalancer::idealLoad_(uint64_t now) {
	uint64_t systemLoad = 0;
	size_t numOnline = 0;
	for (size_t i = 0; i < getCpuCount(); ++i) {
		auto *node = &lbNode.getFor(i);
		if (!node->online.load(std::memory_order_acquire))
			continue;
		systemLoad += node->currentLoad(now).load();
		++numOnline;
	}
	assert(numOnline);
	return systemLoad / numOnline;
}

template<typename F>
void LoadBalancer::forEachThread_(LbNode *node, F fn) {
	// The referenced control block keeps our position in the list across RCU critical sections.
	smarter::shared_ptr<LbControlBlock> cursor;
	while (true) {
		smarter::shared_ptr<LbControlBlock> next;
		{
			IplGuard<ipl::noSchedule> rcuGuard;

			auto it = node->tasks.begin();
			if (cursor) {
				it = node->tasks.iterator_to(cursor.get());
				++it;
			}
			while (it != node->tasks.end()) {
				auto *cb = *it;
				++it;
				// The control block may be in the process of being unlinked.
				next = cb->self_.lock();
				if (next)
					break;
			}
		}
		// Dropping the old cursor may unlink it, hence this is done outside of the RCU critical section.
		cursor = std::move(next);
		if (!cursor)
			return;

		// The thread may be in the process of being destructed.
		auto thread = cursor->thread_.lock();
		if (!thread)
			continue;
		// Skip control blocks of threads that moved away from this node.
		if (thread->_lbState.cb_.load(std::memory_order_acquire) != cursor.get())
			continue;

		if (!fn(thread, cursor.get()))
			return;
	}
}

void LoadBalancer::requestPull_(LbNode *srcNode) {
	auto numCpus = getCpuCount();
	size_t srcIndex = srcNode->cpu->cpuIndex;

	auto now = getClockNanos();
	auto idealLoad = idealLoad_(now);
	auto srcLoad = srcNode->currentLoad(now).load();
	if (srcLoad <= idealLoad + lbMargin)
		return;

	// Only wake up a CPU if there is a thread that it can pull. Take the first such thread;
	// the CPU that pulls decides on its own which threads it moves.
	size_t dstIndex = noCpu;
	forEachThread_(srcNode, [&] (smarter::shared_ptr<Thread> &thread, LbControlBlock *) -> bool {
		auto load = thread->load().at(now).runnable;
		if (load < lbMargin)
			return true;
		auto *state = &thread->_lbState;

		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&state->mutex_);

		// Among the CPUs that the thread may run on, prefer the least loaded one.
		// Start at the next CPU such that the CPUs do not all favor the ones with small indices.
		uint64_t bestLoad = 0;
		for (size_t k = 1; k < numCpus; ++k) {
			auto i = (srcIndex + k) % numCpus;
			auto *node = &lbNode.getFor(i);
			if (!node->online.load(std::memory_order_acquire) || !state->inAffinityMask_(i))
				continue;
			auto dstLoad = node->currentLoad(now).load();
			if (dstLoad + lbMargin >= idealLoad)
				continue;
			if (!improvesBalance(srcLoad, dstLoad, load))
				continue;
			if (dstIndex == noCpu || dstLoad < bestLoad) {
				dstIndex = i;
				bestLoad = dstLoad;
			}
		}
		return dstIndex == noCpu;
	});
	if (dstIndex == noCpu)
		return;

	if (debugLb)
		infoLogger() << "CPU #" << dstIndex << " is asked to pull from CPU #"
				<< srcIndex << frg::endlog;

	// Requests are only hints: if another request overwrites this one, the CPU still pulls.
	auto *dstNode = &lbNode.getFor(dstIndex);
	dstNode->pullSource.store(srcIndex, std::memory_order_release);
	dstNode->pullEvent.raise();
}

void LoadBalancer::pull_(LbNode *dstNode, size_t hintIndex) {
	auto numCpus = getCpuCount();
	size_t dstIndex = dstNode->cpu->cpuIndex;

	size_t tried[lbMaxSources];
	size_t numTried = 0;
	while (numTried < lbMaxSources) {
		auto now = getClockNanos();
		auto idealLoad = idealLoad_(now);
		auto dstLoad = dstNode->currentLoad(now).load();
		if (dstLoad + lbMargin >= idealLoad)
			break;

		// The CPU that asked us to pull found a thread that we can take. If that thread is gone,
		// try the most loaded CPUs instead of going back to sleep.
		bool isHint = !numTried;
		size_t srcIndex = noCpu;
		uint64_t srcLoad = 0;
		if (isHint) {
			srcIndex = hintIndex;
			srcLoad = lbNode.getFor(hintIndex).currentLoad(now).load();
		} else {
			for (size_t i = 0; i < numCpus; ++i) {
				auto *node = &lbNode.getFor(i);
				if (i == dstIndex || !node->online.load(std::memory_order_acquire))
					continue;
				bool wasTried = false;
				for (size_t j = 0; j < numTried; ++j)
					wasTried = wasTried || tried[j] == i;
				if (wasTried)
					continue;
				auto load = node->currentLoad(now).load();
				if (srcIndex == noCpu || load > srcLoad) {
					srcIndex = i;
					srcLoad = load;
				}
			}
			if (srcIndex == noCpu)
				break;
		}
		tried[numTried++] = srcIndex;

		if (srcLoad <= idealLoad + lbMargin) {
			if (isHint)
				continue;
			// All remaining CPUs have even less load.
			break;
		}
		auto *srcNode = &lbNode.getFor(srcIndex);

		auto amount = frg::min(srcLoad - idealLoad, idealLoad - dstLoad);
		if (debugLb)
			infoLogger() << "CPU #" << dstIndex << " pulls " << amount
					<< " from CPU #" << srcIndex << frg::endlog;

		if (pullFrom_(srcNode, dstNode, amount)) {
			// Other CPUs may be able to pull even more.
			// Pass the request on instead of waiting until the source asks again.
			// This may ask this CPU again if it is still the least loaded one; it then pulls once more.
			requestPull_(srcNode);
			break;
		}
	}
}

uint64_t LoadBalancer::pullFrom_(LbNode *srcNode, LbNode *dstNode, uint64_t amount) {
	// Replacement control block for the next migration, allocated outside of all locks.
	smarter::shared_ptr<LbControlBlock> spare;
	uint64_t movedLoad = 0;
	auto visit = [&] (smarter::shared_ptr<Thread> &thread, LbControlBlock *cb, bool onlyWaiting) -> bool {
		if (movedLoad >= amount)
			return false;

		auto now = getClockNanos();
		auto threadLoad = thread->load().at(now);
		if (onlyWaiting && (!threadLoad.isRunnable || threadLoad.isRunning))
			return true;
		auto load = threadLoad.runnable;
		if (load < lbMargin)
			return true;

		// Except for the first thread, do not overshoot the amount by more than we undershoot.
		if (movedLoad && load >= 2 * (amount - movedLoad))
			return true;

		if (!improvesBalance(srcNode->currentLoad(now).load(),
				dstNode->currentLoad(now).load(), load))
			return true;
		auto *state = &thread->_lbState;

		if (!spare)
			spare = LbControlBlock::create();

		bool moved = false;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto pullLock = frg::guard(&srcNode->pullMutex);

			// Other CPUs may have pulled from srcNode since the check above.
			if (improvesBalance(srcNode->currentLoad(now).load(),
					dstNode->currentLoad(now).load(), load)) {
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
		}
		if (moved) {
			movedLoad += load;
			// Notify the thread such that it eventually moves to its assigned CPU.
			Thread::migrateOther(thread);
		}
		return true;
	};

	// Prefer threads that wait for the source CPU; these are the threads that gain from the move.
	// Other threads are only moved to even out the load that they cause when they run again.
	forEachThread_(srcNode, [&] (smarter::shared_ptr<Thread> &thread, LbControlBlock *cb) -> bool {
		return visit(thread, cb, true);
	});
	if (!movedLoad) {
		forEachThread_(srcNode, [&] (smarter::shared_ptr<Thread> &thread, LbControlBlock *cb) -> bool {
			return visit(thread, cb, false);
		});
	}
	return movedLoad;
}

} // namespace thor
