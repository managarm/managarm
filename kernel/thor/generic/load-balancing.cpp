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

template<typename F>
void withOrderedNodeLocks(LbNode *a, LbNode *b, F &&f) {
	if(a == b) {
		auto lock = frg::guard(&a->mutex);
		f();
		return;
	}

	if(a->cpu->cpuIndex < b->cpu->cpuIndex) {
		auto aLock = frg::guard(&a->mutex);
		auto bLock = frg::guard(&b->mutex);
		f();
	} else {
		auto bLock = frg::guard(&b->mutex);
		auto aLock = frg::guard(&a->mutex);
		f();
	}
}

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
	spawnOnWorkQueue(*kernelAlloc, cpu->generalWorkQueue, loadBalancer->run_(cpu));
}

void LoadBalancer::connect(Thread *thread, CpuData *cpu) {
	assert(!thread->_lbCb);
	auto *node = &lbNode.get(cpu);

	auto cb = frg::construct<LbControlBlock>(*kernelAlloc, thread);

	// Publish the control block only after its initial list membership exists.
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&node->mutex);

		node->tasks.push_back(cb);
		cb->setAssignedCpu(cpu);
		cb->node_.store(node, std::memory_order_release);
	}
	thread->_lbCb = cb;
}

void LoadBalancer::setAffinity(Thread *thread, frg::span<const uint8_t> mask) {
	assert(thread->_lbCb);
	assert(mask.size() == LbControlBlock::affinityMaskSize());
	assert(LbControlBlock::findFirstCpu(mask) != static_cast<size_t>(-1));

	auto *cb = thread->_lbCb;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto setterLock = frg::guard(&cb->affinitySetterMutex_);
		cb->affinityUpdateActive_.store(true, std::memory_order_release);

		// Wait out a balancing commit that passed its active check immediately
		// before the setter published the flag. Do not carry this lock into the
		// node acquisition below.
		{
			auto metadataLock = frg::guard(&cb->mutex_);
		}

		auto *oldNode = cb->getNode();
		auto *oldAssignedCpu = cb->getAssignedCpu();
		auto *assignedCpu = oldAssignedCpu;
		if(!assignedCpu || !(mask[assignedCpu->cpuIndex / 8]
				& (1 << (assignedCpu->cpuIndex % 8))))
			assignedCpu = getCpuData(LbControlBlock::findFirstCpu(mask));
		auto *newNode = &lbNode.get(assignedCpu);

		withOrderedNodeLocks(oldNode, newNode, [&] {
			auto metadataLock = frg::guard(&cb->mutex_);

			// The setter mutex and quiescence barrier make either mismatch a
			// protocol violation rather than a retry case.
			assert(cb->getNode() == oldNode);
			assert(cb->getAssignedCpu() == oldAssignedCpu);
			assert(cb->affinityUpdateActive_.load(std::memory_order_acquire));

			cb->setAffinityMaskLocked(mask);
			assert(oldNode->currentLoad >= cb->load_);
			if(oldNode != newNode) {
				oldNode->tasks.erase(oldNode->tasks.iterator_to(cb));
				oldNode->currentLoad -= cb->load_;
				newNode->tasks.push_back(cb);
				newNode->currentLoad += cb->load_;
			}
			cb->setAssignedCpu(assignedCpu);
			cb->node_.store(newNode, std::memory_order_release);
		});

		cb->affinityUpdateActive_.store(false, std::memory_order_release);
	}

	// Raising a condition takes the thread mutex, while accounting can take it
	// after a node mutex. Keep this outside of all load-balancer locks.
	Thread::migrateOther(thread->self);
}

coroutine<void> LoadBalancer::run_(CpuData *cpu) {
	auto *thisNode = &lbNode.get(cpu);

	bool joined = false;
	uint64_t lastDecay = getClockNanos();
	size_t balanceOffset = 1;
	struct AccountingEntry {
		LbControlBlock *cb;
		smarter::shared_ptr<Thread> thread;
		uint64_t load;
	};
	frg::vector<AccountingEntry, KernelAlloc> entries{*kernelAlloc};

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
		frg::intrusive_list<
			LbControlBlock,
			frg::locate_member<
				LbControlBlock,
				frg::default_list_hook<LbControlBlock>,
				&LbControlBlock::hook_
			>
		> staleCbs;
		// Size the snapshot without retaining the node lock across allocation.
		entries.resize(0);
		size_t snapshotCapacity = 0;
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
					staleCbs.push_back(cb);
					continue;
				}
				++snapshotCapacity;
			}
		}
		entries.resize(snapshotCapacity);

		// Pin a best-effort snapshot. Threads that arrive after sizing retain
		// their previous load until the next round.
		size_t numEntries = 0;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&thisNode->mutex);

			for(auto *cb : thisNode->tasks) {
				if(numEntries == entries.size())
					break;
				if(auto thread = cb->thread_.lock())
					entries[numEntries++] = AccountingEntry{cb, std::move(thread), 0};
			}
		}

		// Thread load updates take Thread::_mutex. Keep them outside of the node
		// lock so affinity changes and ownership commits are not serialized here.
		for(size_t i = 0; i < numEntries; ++i) {
			entries[i].thread->updateLoad(applyDecay, lbDecay, 8);
			entries[i].load = entries[i].thread->loadLevel();
		}

		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&thisNode->mutex);

			for(size_t i = 0; i < numEntries; ++i)
				if(entries[i].cb->getNode() == thisNode)
					entries[i].cb->load_ = entries[i].load;

			auto it = thisNode->tasks.begin();
			while(it != thisNode->tasks.end()) {
				auto currentIt = it++;
				auto *cb = *currentIt;
				if(!cb->thread_.lock()) {
					thisNode->tasks.erase(currentIt);
					staleCbs.push_back(cb);
					continue;
				}
				load += cb->load_;
			}

			// This is an accounting snapshot. Transfers keep currentLoad live,
			// while totalLoad is only used to derive this round's ideal load.
			thisNode->totalLoad = load;
			thisNode->currentLoad = load;
		}
		entries.resize(0);

		// Destroy stale CBs outside of locks.
		while(!staleCbs.empty())
			frg::destruct(*kernelAlloc, staleCbs.pop_front());

		if (debugLb)
			infoLogger() << "CPU #" << cpu->cpuIndex << " has load " << load << frg::endlog;

		// Global barrier to wait until all CPUs know their load level.
		co_await barrier_.async_wait(barrier_.arrive());

		// Sum load once, then publish it to all CPUs through the barrier.
		if (!cpu->cpuIndex) {
			systemLoad_ = 0;
			for (size_t i = 0; i < getCpuCount(); ++i)
				systemLoad_ += lbNode.getFor(i).totalLoad;
		}
		co_await barrier_.async_wait(barrier_.arrive());

		auto systemLoad = systemLoad_;
		uint64_t idealLoad = systemLoad / getCpuCount();
		if (debugLb && cpu == getCpuData(0))
			infoLogger() << "Total system load is " << systemLoad
					<< " (ideal load: " << idealLoad << ")" << frg::endlog;

		auto numCpus = getCpuCount();
		if (enableLb && numCpus > 1) {
			// Visit one source per destination and round. Rotating the offset covers
			// every directed CPU pair over numCpus - 1 rounds without quadratic work.
			auto sourceIndex = (cpu->cpuIndex + balanceOffset) % numCpus;
			balanceBetween_(&lbNode.getFor(sourceIndex), thisNode, idealLoad);

			if (++balanceOffset == numCpus)
				balanceOffset = 1;
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

	// Tunable: maximum tasks considered for one CPU-pair commit. Larger batches
	// can converge faster, at the cost of stack space and longer lock hold times.
	constexpr size_t candidateCapacity = 8;
	// Tunable: maximum source-list entries inspected per CPU-pair pass. Larger
	// scans find sparse candidates sooner but increase IRQ-disabled lock time.
	constexpr size_t scanCapacity = 64;
	struct Candidate {
		LbControlBlock *cb;
		smarter::shared_ptr<Thread> thread;
	};
	frg::array<Candidate, candidateCapacity> candidates;
	frg::array<bool, candidateCapacity> committed{};
	size_t numCandidates = 0;

	uint64_t simulatedDstLoad = 0;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto dstLock = frg::guard(&dstNode->mutex);
		simulatedDstLoad = dstNode->currentLoad;
	}

	// Select a bounded batch under only the source node lock. Strong thread
	// references pin selected control blocks through notification below. Rotate
	// visited entries to keep later entries reachable across bounded scans.
	{
		auto irqLock = frg::guard(&irqMutex());
		auto srcLock = frg::guard(&srcNode->mutex);
		auto simulatedSrcLoad = srcNode->currentLoad;
		auto *first = srcNode->tasks.empty() ? nullptr : srcNode->tasks.front();
		size_t numScanned = 0;

		while (!srcNode->tasks.empty() && numScanned < scanCapacity) {
			// Do not attempt to do load balancing if source and destination are both
			// undersubscribed. While it may still be possible to improve the balance,
			// it is probably not worth it in terms of effort and cache degradation.
			if (simulatedSrcLoad < idealLoad && simulatedDstLoad < idealLoad)
				break;

			auto *cb = srcNode->tasks.pop_front();
			srcNode->tasks.push_back(cb);
			if(numScanned++ && cb == first)
				break;

			// Do not move threads with tiny contributions to the total load.
			if (!cb->load_)
				continue;

			if (cb->affinityUpdateActive_.load(std::memory_order_acquire))
				continue;

			auto metadataLock = frg::guard(&cb->mutex_);
			if (cb->affinityUpdateActive_.load(std::memory_order_acquire))
				continue;
			if (!cb->inAffinityMaskLocked(dstNode->cpu->cpuIndex))
				continue;
			if (!improvesBalance(simulatedSrcLoad, simulatedDstLoad, cb->load_))
				continue;

			if(auto thread = cb->thread_.lock()) {
				assert(numCandidates < candidateCapacity);
				candidates[numCandidates++] = Candidate{cb, std::move(thread)};
				simulatedSrcLoad -= cb->load_;
				simulatedDstLoad += cb->load_;
				if(numCandidates == candidateCapacity)
					break;
			}
		}
	}

	// Revalidate and commit at most this one batch. Stale selections are
	// rejected until the next balancing round instead of being retried.
	{
		auto irqLock = frg::guard(&irqMutex());
		withOrderedNodeLocks(srcNode, dstNode, [&] {
			for(size_t i = 0; i < numCandidates; ++i) {
				auto *cb = candidates[i].cb;
				auto metadataLock = frg::guard(&cb->mutex_);

				if(cb->affinityUpdateActive_.load(std::memory_order_acquire))
					continue;
				if(cb->getNode() != srcNode || !cb->load_)
					continue;
				if(!cb->inAffinityMaskLocked(dstNode->cpu->cpuIndex))
					continue;
				if(srcNode->currentLoad < cb->load_)
					panicLogger() << "thor: load-balancer source load underflow" << frg::endlog;
				if(!improvesBalance(srcNode->currentLoad, dstNode->currentLoad, cb->load_))
					continue;

				if(debugLb)
					infoLogger() << "Moving thread with load " << cb->load_
							<< " from CPU " << srcNode->cpu->cpuIndex
							<< " to CPU " << dstNode->cpu->cpuIndex << frg::endlog;

				srcNode->tasks.erase(srcNode->tasks.iterator_to(cb));
				srcNode->currentLoad -= cb->load_;
				dstNode->tasks.push_back(cb);
				dstNode->currentLoad += cb->load_;
				cb->setAssignedCpu(dstNode->cpu);
				cb->node_.store(dstNode, std::memory_order_release);
				committed[i] = true;
			}
		});
	}

	// migrateOther() can take Thread::_mutex; do not invert accounting's
	// node -> Thread::_mutex order.
	for(size_t i = 0; i < numCandidates; ++i)
		if(committed[i])
			Thread::migrateOther(candidates[i].thread);
}

} // namespace thor
