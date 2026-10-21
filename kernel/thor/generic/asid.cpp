#include <thor-internal/arch-generic/asid.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/arch-generic/paging-consts.hpp>
#include <thor-internal/arch-generic/paging.hpp>

#include <thor-internal/cpu-data.hpp>
#include <thor-internal/metrics.hpp>

namespace thor {

THOR_DEFINE_PERCPU(asidData);

namespace {

void invalidateNode(int asid, ShootNode *node) {
	// If we're invalidating a lot of pages, just invalidate the
	// whole ASID instead.
	// invalidateAsid(globalBindingId) is not allowed, so avoid
	// the optimization in that case.
	if(asid != globalBindingId && (node->size >> kPageShift) >= 64) {
		invalidateAsid(asid);
	} else {
		for(size_t off = 0; off < node->size; off += kPageSize)
			invalidatePage(asid, reinterpret_cast<void *>(node->address + off));
	}
}

} // namespace anonymous

template<typename F>
uint64_t PageBinding::processShootdowns_(PageSpace *space,
		uint64_t afterSequence, uint64_t upToSequence, F invalidate) {
	assert(!intsAreEnabled());

	// In the code below, note that we cannot assume that nodes are processed in
	// strict FIFO order since there may be ShootNodes initiated by this CPU that are
	// shot down earlier (and hence removed from the queue) than the current node.

	// Find the first unprocessed node by scanning backward from the back of the queue.
	// We may miss nodes that are concurrently removed but that does not impact correctness.
	ShootNode *current = space->shootQueue_.back();
	if(!current || current->sequence_ <= afterSequence)
		return afterSequence;
	while(true) {
		auto prev = current->queueNode.previous.load(std::memory_order_acquire);
		if(!prev || prev->sequence_ <= afterSequence)
			break;
		current = prev;
	}

	// TODO: If we see too many pages during the backwards traversal above,
	//       we could simply invalidate the entire space and mark everything as invalidated
	//       on the forward pass below.

	auto lastSequence = afterSequence;
	while(current) {
		auto next = current->queueNode.next.load(std::memory_order_acquire);
		auto seq = current->sequence_;
		if(seq > upToSequence)
			break;

		if(current->initiatorCpu_ != getCpuData()) {
			invalidate(current);

			if(current->bindingsToShoot_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
				{
					auto lock = frg::guard(&space->mutex_);
					space->shootQueue_.erase(current);
				}
				current->complete();
			}
		}

		lastSequence = seq;
		current = next;
	}
	return lastSequence;
}

void
PageBinding::doShootdown_(PageSpace *space) {
	alreadyShotSequence_ = processShootdowns_(space, alreadyShotSequence_, UINT64_MAX,
			[&] (ShootNode *node) { invalidateNode(id_, node); });
}

// Drain the shootdown queue without actually invalidating mappings.
// This is called after a space was unbound.
void
PageBinding::drainShootdown_(PageSpace *space, uint64_t afterSequence, uint64_t upToSequence) {
	processShootdowns_(space, afterSequence, upToSequence, [] (ShootNode *) { });
}

bool PageBinding::isPrimary() {
	assert(!intsAreEnabled());
	auto &context = asidData.get()->pageContext;

	return context.primaryBinding_ == this;
}

void PageBinding::makeLazy_() {
	assert(!intsAreEnabled());
	assert(boundSpace_ && !lazy_);
	assert(id_ != globalBindingId);

	uint64_t upToSequence;
	{
		auto lock = frg::guard(&boundSpace_->mutex_);
		upToSequence = boundSpace_->shootSequence_;
		boundSpace_->numActiveBindings_--;
		boundSpace_->activeCpus_.reset(getCpuData()->cpuIndex);
	}

	// Requests up to upToSequence still count this binding, later ones are caught up on in makeActive_().
	processShootdowns_(boundSpace_.get(), alreadyShotSequence_, upToSequence,
			[&] (ShootNode *node) { invalidateNode(id_, node); });
	alreadyShotSequence_ = upToSequence;
	lazy_ = true;
}

bool PageBinding::makeActive_() {
	assert(!intsAreEnabled());
	assert(boundSpace_ && lazy_);

	uint64_t sequence;
	{
		auto lock = frg::guard(&boundSpace_->mutex_);
		sequence = boundSpace_->shootSequence_;
		boundSpace_->numActiveBindings_++;
		boundSpace_->activeCpus_.set(getCpuData()->cpuIndex);
	}

	auto stale = sequence != alreadyShotSequence_;
	if(stale)
		staleBindingFlushesCounter.add();
	alreadyShotSequence_ = sequence;
	lazy_ = false;
	return stale;
}

void PageBinding::makePrimary_() {
	auto &context = asidData.get()->pageContext;

	// Only the primary binding can be in use, hence the previous one stops taking part in shootdowns.
	auto previous = context.primaryBinding_;
	if(previous && previous != this && previous->boundSpace_ && !previous->lazy_)
		previous->makeLazy_();

	primaryStamp_ = context.nextStamp_++;
	context.primaryBinding_ = this;
}

void PageBinding::rebind() {
	assert(!intsAreEnabled());
	assert(boundSpace_);
	// Bindings other than the primary one are always lazy.
	assert(lazy_);

	// The global binding should always be current
	assert(id_ != globalBindingId);
	auto stale = makeActive_();
	switchToPageTable(boundSpace_->rootTable(), id_, stale);

	makePrimary_();
}

void PageBinding::rebind(smarter::shared_ptr<PageSpace> space) {
	assert(!intsAreEnabled());
	assert(!boundSpace_ || boundSpace_.get() != space.get()); // This would be unnecessary work.
	assert(id_ != globalBindingId);

	// Disallow mapping the kernel page space to the ASID bindings.
	assert(space.get() != &KernelPageSpace::global());

	auto unboundSpace = boundSpace_;
	auto unboundSequence = alreadyShotSequence_;
	auto unboundLazy = lazy_;

	// Bind the new space.
	uint64_t targetSeq;
	{
		auto lock = frg::guard(&space->mutex_);

		targetSeq = space->shootSequence_;
		space->numBindings_++;
		space->numActiveBindings_++;
		space->boundCpus_.set(getCpuData()->cpuIndex);
		space->activeCpus_.set(getCpuData()->cpuIndex);
	}

	boundSpace_ = space;
	alreadyShotSequence_ = targetSeq;
	lazy_ = false;

	switchToPageTable(boundSpace_->rootTable(), id_, true);

	makePrimary_();

	// Mark every shootdown request in the unbound space as shot-down.
	if(unboundSpace) {
		uint64_t upToSequence;
		RetireNode *retireNode = nullptr;
		{
			auto lock = frg::guard(&unboundSpace->mutex_);
			upToSequence = unboundSpace->shootSequence_;
			unboundSpace->numBindings_--;
			unboundSpace->boundCpus_.reset(getCpuData()->cpuIndex);
			if(!unboundLazy) {
				unboundSpace->numActiveBindings_--;
				unboundSpace->activeCpus_.reset(getCpuData()->cpuIndex);
			}
			if(!unboundSpace->numBindings_ && unboundSpace->retireNode_) {
				retireNode = unboundSpace->retireNode_;
				unboundSpace->retireNode_ = nullptr;
			}
		}

		// No request counts lazy bindings.
		if(!unboundLazy)
			drainShootdown_(unboundSpace.get(), unboundSequence, upToSequence);

		if(retireNode)
			retireNode->complete();
	}
}

void PageBinding::initialBind(smarter::shared_ptr<PageSpace> space) {
	assert(!intsAreEnabled());
	assert(!boundSpace_);
	assert(id_ == globalBindingId);
	assert(space.get() == &KernelPageSpace::global());

	// Bind the new space.
	uint64_t targetSeq;
	{
		auto lock = frg::guard(&space->mutex_);

		targetSeq = space->shootSequence_;
		space->numBindings_++;
		space->numActiveBindings_++;
	}

	boundSpace_ = space;
	alreadyShotSequence_ = targetSeq;
}

void PageBinding::unbind() {
	assert(!intsAreEnabled());
	assert(id_ != globalBindingId);

	if(!boundSpace_)
		return;

	// Perform shootdown.
	if(isPrimary()) {
		// If this is the primary binding, switch away, as the
		// page tables are about to be freed after this is
		// complete.
		switchAwayFromPageTable(id_);
	} else {
		invalidateAsid(id_);
	}

	uint64_t upToSequence;
	RetireNode *retireNode = nullptr;
	{
		auto lock = frg::guard(&boundSpace_->mutex_);
		upToSequence = boundSpace_->shootSequence_;
		boundSpace_->numBindings_--;
		boundSpace_->boundCpus_.reset(getCpuData()->cpuIndex);
		if(!lazy_) {
			boundSpace_->numActiveBindings_--;
			boundSpace_->activeCpus_.reset(getCpuData()->cpuIndex);
		}
		if(!boundSpace_->numBindings_ && boundSpace_->retireNode_) {
			retireNode = boundSpace_->retireNode_;
			boundSpace_->retireNode_ = nullptr;
		}
	}

	// No request counts lazy bindings.
	if(!lazy_)
		drainShootdown_(boundSpace_.get(), alreadyShotSequence_, upToSequence);

	if(retireNode)
		retireNode->complete();

	boundSpace_ = nullptr;
	alreadyShotSequence_ = 0;
	lazy_ = false;
}

void PageBinding::shootdown() {
	assert(!intsAreEnabled());

	if(!boundSpace_)
		return;

	// If we retire the space anyway, just flush the whole ASID.
	if(boundSpace_->wantToRetire_.load(std::memory_order_acquire)) {
		unbind();
		return;
	}

	// Lazy bindings catch up in makeActive_().
	if(lazy_)
		return;

	doShootdown_(boundSpace_.get());
}


void PageSpace::activate(smarter::shared_ptr<PageSpace> space) {
	auto &bindings = asidData.get()->bindings;

	size_t lruIdx = 0;
	for(size_t i = 0; i < bindings.size(); i++) {
		// If the space is currently bound, always keep that binding.
		auto bound = bindings[i].boundSpace();
		if(bound && bound.get() == space.get()) {
			if(!bindings[i].isPrimary()) {
				bindings[i].rebind();
			} else if(bindings[i].lazy_) {
				// The page tables are still loaded, but the TLB may hold entries that were shot down.
				if(bindings[i].makeActive_())
					invalidateAsid(bindings[i].id());
			}
			return;
		}

		// Otherwise, prefer the LRU binding.
		if(bindings[i].primaryStamp() < bindings[lruIdx].primaryStamp())
			lruIdx = i;
	}

	bindings[lruIdx].rebind(space);
}

void PageSpace::enterLazyMode() {
	assert(!intsAreEnabled());

	auto binding = asidData.get()->pageContext.primaryBinding_;
	if(binding && binding->boundSpace_ && !binding->lazy_)
		binding->makeLazy_();
}

void PageSpace::deactivateAll() {
	assert(!intsAreEnabled());

	for(auto &binding : asidData.get()->bindings)
		binding.unbind();
}


PageSpace::PageSpace(PhysicalAddr rootTable)
: rootTable_{rootTable}, numBindings_{0}, boundCpus_{getCpuCount(), *kernelAlloc},
		numActiveBindings_{0}, activeCpus_{getCpuCount(), *kernelAlloc}, shootSequence_{0} { }

PageSpace::PageSpace(PhysicalAddr rootTable, KernelSpaceTag)
: rootTable_{rootTable}, numBindings_{0}, numActiveBindings_{0}, shootSequence_{0} { }

PageSpace::~PageSpace() {
	assert(!numBindings_);
	assert(boundCpus_.none());
	assert(!numActiveBindings_);
	assert(activeCpus_.none());
}


void PageSpace::retire(RetireNode *node) {
	// The kernel space is never retired and hence does not track boundCpus_.
	assert(this != &KernelPageSpace::global());

	frg::dyn_bitset<KernelAlloc> targets{getCpuCount(), *kernelAlloc};

	bool anyBindings;
	{
		// PageBinding::unbind() below requires IRQs to be disabled.
		auto irqLock = frg::guard(&irqMutex());

		{
			auto lock = frg::guard(&mutex_);

			anyBindings = numBindings_;
			if(anyBindings) {
				retireNode_ = node;
				wantToRetire_.store(true, std::memory_order_release);
			}
			assert(boundCpus_.count() == numBindings_);
			assert(activeCpus_.count() == numActiveBindings_);
			targets.assign(boundCpus_);
		}

		// Perform synchronous unbinding.
		auto &bindings = asidData.get()->bindings;
		for(size_t i = 0; i < bindings.size(); i++) {
			if(bindings[i].boundSpace().get() != this)
				continue;

			bindings[i].unbind();
			assert(targets.test(getCpuData()->cpuIndex));
			targets.reset(getCpuData()->cpuIndex);
		}
	}

	if(!anyBindings)
		node->complete();

	assert(!targets.test(getCpuData()->cpuIndex));
	retireTargetsCounter.add(targets.count());
	sendShootdownIpi(targets);
}


bool PageSpace::submitShootdown(ShootNode *node) {
	assert(!(node->address & (kPageSize - 1)));
	assert(!(node->size & (kPageSize - 1)));

	// The kernel space is held by every CPU, hence the broadcast IPI is the cheapest option.
	// The zero-sized bitset is required to avoid recursive calls from shootdown to the allocator.
	bool global = this == &KernelPageSpace::global();
	frg::dyn_bitset<KernelAlloc> targets{global ? 0 : getCpuCount(), *kernelAlloc};
	if(!global)
		userShootdownsCounter.add();

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		auto unshotBindings = numActiveBindings_;

		auto &bindings = asidData.get()->bindings;

		// Perform synchronous shootdown.
		PageBinding *lazyBinding = nullptr;
		if(global) {
			assert(unshotBindings);
			invalidateNode(globalBindingId, node);
			unshotBindings--;
		} else {
			assert(boundCpus_.count() == numBindings_);
			assert(activeCpus_.count() == numActiveBindings_);
			targets.assign(activeCpus_);

			for(size_t i = 0; i < bindings.size(); i++) {
				if(bindings[i].boundSpace().get() != this)
					continue;

				if(bindings[i].lazy_) {
					// A stale lazy binding flushes its whole ASID in makeActive_() anyway.
					// A non-stale lazy binding on the current CPU is cheap to keep in sync,
					// so invalidate here and advance alreadyShotSequence_.
					if(bindings[i].alreadyShotSequence_ == shootSequence_) {
						invalidateNode(bindings[i].id(), node);
						lazyBinding = &bindings[i];
					}
					continue;
				}
				invalidateNode(bindings[i].id(), node);
				assert(unshotBindings);
				unshotBindings--;
				assert(targets.test(getCpuData()->cpuIndex));
				targets.reset(getCpuData()->cpuIndex);
			}
		}

		// Lazy bindings only notice requests through the sequence, so advance it even if nothing is queued.
		auto sequence = ++shootSequence_;
		if(lazyBinding)
			lazyBinding->alreadyShotSequence_ = sequence;

		if(!unshotBindings)
			return true;

		node->initiatorCpu_ = getCpuData();
		node->sequence_ = sequence;
		node->bindingsToShoot_ = unshotBindings;
		shootQueue_.push_back(node);
	}

	if(global) {
		sendShootdownIpi();
	} else {
		assert(!targets.test(getCpuData()->cpuIndex));
		remoteUserShootdownsCounter.add();
		userShootdownTargetsCounter.add(targets.count());
		sendShootdownIpi(targets);
	}
	return false;
}


} // namespace thor
