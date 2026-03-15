#include <thor-internal/arch-generic/asid.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/arch-generic/paging-consts.hpp>
#include <thor-internal/arch-generic/paging.hpp>

#include <thor-internal/cpu-data.hpp>

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

void
PageBinding::doShootdown_(PageSpace *space) {
	assert(!intsAreEnabled());

	// In the code below, note that we cannot assume that nodes are processed in
	// strict FIFO order since there may be ShootNodes initiated by this CPU that are
	// shot down earlier (and hence removed from the queue) than the current node.

	// Find the first unprocessed node by scanning backward from the back of the queue.
	// We may miss nodes that are concurrently removed but that does not impact correctness.
	ShootNode *current = space->shootQueue_.back();
	if(!current || current->sequence_ <= alreadyShotSequence_)
		return;
	while(true) {
		auto prev = current->queueNode.previous.load(std::memory_order_acquire);
		if(!prev || prev->sequence_ <= alreadyShotSequence_)
			break;
		current = prev;
	}

	// TODO: If we see too many pages during the backwards traversal above,
	//       we could simply invalidate the entire space and mark everything as invalidated
	//       on the forward pass below.

	while(current) {
		auto next = current->queueNode.next.load(std::memory_order_acquire);
		auto seq = current->sequence_;

		if(current->initiatorCpu_ != getCpuData()) {
			invalidateNode(id_, current);

			if(current->bindingsToShoot_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
				{
					auto lock = frg::guard(&space->mutex_);
					space->shootQueue_.erase(current);
				}
				current->complete();
			}
		}

		alreadyShotSequence_ = seq;
		current = next;
	}
}

// Drain the shootdown queue without actually invalidating mappings.
// This is called after a space was unbound.
void
PageBinding::drainShootdown_(PageSpace *space, uint64_t afterSequence, uint64_t upToSequence) {
	assert(!intsAreEnabled());

	// Same backwards iteration as in doShootdown_().
	ShootNode *current = space->shootQueue_.back();
	if(!current || current->sequence_ <= afterSequence)
		return;
	while(true) {
		auto prev = current->queueNode.previous.load(std::memory_order_acquire);
		if(!prev || prev->sequence_ <= afterSequence)
			break;
		current = prev;
	}

	while(current) {
		auto next = current->queueNode.next.load(std::memory_order_acquire);
		auto seq = current->sequence_;
		if (seq > upToSequence)
			break;

		if(current->initiatorCpu_ != getCpuData()) {
			if(current->bindingsToShoot_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
				{
					auto lock = frg::guard(&space->mutex_);
					space->shootQueue_.erase(current);
				}
				current->complete();
			}
		}

		current = next;
	}
}

bool PageBinding::isPrimary() {
	assert(!intsAreEnabled());
	auto &context = asidData.get()->pageContext;

	return context.primaryBinding_ == this;
}

void PageBinding::rebind() {
	assert(!intsAreEnabled());
	assert(boundSpace_);
	auto &context = asidData.get()->pageContext;

	// The global binding should always be current
	assert(id_ != globalBindingId);
	switchToPageTable(boundSpace_->rootTable(), id_, false);

	primaryStamp_ = context.nextStamp_++;
	context.primaryBinding_ = this;
}

void PageBinding::rebind(smarter::shared_ptr<PageSpace> space) {
	assert(!intsAreEnabled());
	assert(!boundSpace_ || boundSpace_.get() != space.get()); // This would be unnecessary work.
	assert(id_ != globalBindingId);
	auto &context = asidData.get()->pageContext;

	// Disallow mapping the kernel page space to the ASID bindings.
	assert(space.get() != &KernelPageSpace::global());

	auto unboundSpace = boundSpace_;
	auto unboundSequence = alreadyShotSequence_;

	// Bind the new space.
	uint64_t targetSeq;
	{
		auto lock = frg::guard(&space->mutex_);

		targetSeq = space->shootSequence_;
		space->numBindings_++;
	}

	boundSpace_ = space;
	alreadyShotSequence_ = targetSeq;

	switchToPageTable(boundSpace_->rootTable(), id_, true);

	primaryStamp_ = context.nextStamp_++;
	context.primaryBinding_ = this;

	// Mark every shootdown request in the unbound space as shot-down.
	if(unboundSpace) {
		uint64_t upToSequence;
		RetireNode *retireNode = nullptr;
		{
			auto lock = frg::guard(&unboundSpace->mutex_);
			upToSequence = unboundSpace->shootSequence_;
			unboundSpace->numBindings_--;
			if(!unboundSpace->numBindings_ && unboundSpace->retireNode_) {
				retireNode = unboundSpace->retireNode_;
				unboundSpace->retireNode_ = nullptr;
			}
		}

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
		if(!boundSpace_->numBindings_ && boundSpace_->retireNode_) {
			retireNode = boundSpace_->retireNode_;
			boundSpace_->retireNode_ = nullptr;
		}
	}

	drainShootdown_(boundSpace_.get(), alreadyShotSequence_, upToSequence);

	if(retireNode)
		retireNode->complete();

	boundSpace_ = nullptr;
	alreadyShotSequence_ = 0;
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

	doShootdown_(boundSpace_.get());
}


void PageSpace::activate(smarter::shared_ptr<PageSpace> space) {
	auto &bindings = asidData.get()->bindings;

	size_t lruIdx = 0;
	for(size_t i = 0; i < bindings.size(); i++) {
		// If the space is currently bound, always keep that binding.
		auto bound = bindings[i].boundSpace();
		if(bound && bound.get() == space.get()) {
			if(!bindings[i].isPrimary())
				bindings[i].rebind();
			return;
		}

		// Otherwise, prefer the LRU binding.
		if(bindings[i].primaryStamp() < bindings[lruIdx].primaryStamp())
			lruIdx = i;
	}

	bindings[lruIdx].rebind(space);
}


PageSpace::PageSpace(PhysicalAddr rootTable)
: rootTable_{rootTable}, numBindings_{0}, shootSequence_{0} { }

PageSpace::~PageSpace() {
	assert(!numBindings_);
}


void PageSpace::retire(RetireNode *node) {
	bool anyBindings;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		anyBindings = numBindings_;
		if(anyBindings) {
			retireNode_ = node;
			wantToRetire_.store(true, std::memory_order_release);
		}
	}

	if(!anyBindings)
		node->complete();

	sendShootdownIpi();
}


bool PageSpace::submitShootdown(ShootNode *node) {
	assert(!(node->address & (kPageSize - 1)));
	assert(!(node->size & (kPageSize - 1)));

	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&mutex_);

		auto unshotBindings = numBindings_;

		auto &bindings = asidData.get()->bindings;

		// Perform synchronous shootdown.
		if(this == &KernelPageSpace::global()) {
			assert(unshotBindings);
			invalidateNode(globalBindingId, node);
			unshotBindings--;
		} else {
			for(size_t i = 0; i < bindings.size(); i++) {
				if(bindings[i].boundSpace().get() != this)
					continue;

				assert(unshotBindings);
				invalidateNode(bindings[i].id(), node);
				unshotBindings--;
			}
		}

		if(!unshotBindings)
			return true;

		node->initiatorCpu_ = getCpuData();
		node->sequence_ = ++shootSequence_;
		node->bindingsToShoot_ = unshotBindings;
		shootQueue_.push_back(node);
	}

	sendShootdownIpi();
	return false;
}


} // namespace thor
