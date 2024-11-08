#include <thor-internal/arch-generic/asid.hpp>

#include <thor-internal/cpu-data.hpp>
#include <thor-internal/arch/ints.hpp>

namespace thor {

bool PageBinding::isPrimary() {
	assert(!intsAreEnabled());
	auto &context = getCpuData()->asidData->pageContext;

	return context.primaryBinding_ == this;
}

void PageBinding::rebind() {
	assert(!intsAreEnabled());
	assert(boundSpace_);
	auto &context = getCpuData()->asidData->pageContext;

	// The global binding should always be current
	assert(id_ != globalBindingId);
	switchToPageTable(boundSpace_->rootTable(), id_, false);

	primaryStamp_ = context.nextStamp_++;
	context.primaryBinding_ = this;
}

void PageBinding::rebind(smarter::shared_ptr<PageSpace> space) {
	assert(!intsAreEnabled());
	assert(!boundSpace_ || boundSpace_.get() != space.get()); // This would be unnecessary work.
	auto &context = getCpuData()->asidData->pageContext;

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

	if(id_ != globalBindingId) {
		switchToPageTable(boundSpace_->rootTable(), id_, true);
	}

	primaryStamp_ = context.nextStamp_++;
	context.primaryBinding_ = this;

	// Mark every shootdown request in the unbound space as shot-down.
	ShootNodeList complete;

	if(unboundSpace) {
		auto lock = frg::guard(&unboundSpace->mutex_);

		if(!unboundSpace->shootQueue_.empty()) {
			auto current = unboundSpace->shootQueue_.back();
			while(current->sequence_ > unboundSequence) {
				auto predecessor = current->queueNode.previous;

				// Signal completion of the shootdown.
				if(current->initiatorCpu_ != getCpuData()) {
					if(current->bindingsToShoot_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
						auto it = unboundSpace->shootQueue_.iterator_to(current);
						unboundSpace->shootQueue_.erase(it);
						complete.push_front(current);
					}
				}

				if(!predecessor)
					break;
				current = predecessor;
			}
		}

		unboundSpace->numBindings_--;
		if(!unboundSpace->numBindings_ && unboundSpace->retireNode_) {
			unboundSpace->retireNode_->complete();
			unboundSpace->retireNode_ = nullptr;
		}
	}

	while(!complete.empty()) {
		auto current = complete.pop_front();
		current->complete();
	}
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

	ShootNodeList complete;

	{
		auto lock = frg::guard(&boundSpace_->mutex_);

		if(!boundSpace_->shootQueue_.empty()) {
			auto current = boundSpace_->shootQueue_.back();
			while(current->sequence_ > alreadyShotSequence_) {
				auto predecessor = current->queueNode.previous;

				// The actual shootdown was done above.
				// Signal completion of the shootdown.
				if(current->initiatorCpu_ != getCpuData()) {
					if(current->bindingsToShoot_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
						auto it = boundSpace_->shootQueue_.iterator_to(current);
						boundSpace_->shootQueue_.erase(it);
						complete.push_front(current);
					}
				}

				if(!predecessor)
					break;
				current = predecessor;
			}
		}

		boundSpace_->numBindings_--;
		if(!boundSpace_->numBindings_ && boundSpace_->retireNode_) {
			boundSpace_->retireNode_->complete();
			boundSpace_->retireNode_ = nullptr;
		}
	}

	boundSpace_ = nullptr;
	alreadyShotSequence_ = 0;

	while(!complete.empty()) {
		auto current = complete.pop_front();
		current->complete();
	}
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

	ShootNodeList complete;

	uint64_t targetSeq;
	{
		auto lock = frg::guard(&boundSpace_->mutex_);

		if(!boundSpace_->shootQueue_.empty()) {
			auto current = boundSpace_->shootQueue_.back();
			while(current->sequence_ > alreadyShotSequence_) {
				auto predecessor = current->queueNode.previous;

				if(current->initiatorCpu_ != getCpuData()) {
					// Perform the actual shootdown.
					for(size_t off = 0; off < current->size; off += kPageSize)
						invalidatePage(id_, reinterpret_cast<void *>(current->address + off));

					// Signal completion of the shootdown.
					if(current->bindingsToShoot_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
						auto it = boundSpace_->shootQueue_.iterator_to(current);
						boundSpace_->shootQueue_.erase(it);
						complete.push_front(current);
					}
				}

				if(!predecessor)
					break;
				current = predecessor;
			}
		}
		targetSeq = boundSpace_->shootSequence_;
	}

	alreadyShotSequence_ = targetSeq;

	while(!complete.empty()) {
		auto current = complete.pop_front();
		current->complete();
	}
}


void PageSpace::activate(smarter::shared_ptr<PageSpace> space) {
	auto &bindings = getCpuData()->asidData->bindings;

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

		auto &globalBinding = getCpuData()->asidData->globalBinding;
		auto &bindings = getCpuData()->asidData->bindings;

		// Perform synchronous shootdown.
		if(globalBinding.boundSpace().get() == this) {
			assert(unshotBindings);
			for(size_t off = 0; off < node->size; off += kPageSize)
				invalidatePage(globalBindingId, reinterpret_cast<void *>(node->address + off));
			unshotBindings--;
		} else {
			for(size_t i = 0; i < bindings.size(); i++) {
				if(bindings[i].boundSpace().get() != this)
					continue;
				assert(unshotBindings);

				// TODO(qookie):
				// if node->size >= 64 pages, invalidate full asid/tlb
				// based on id != -1

				for(size_t off = 0; off < node->size; off += kPageSize)
					invalidatePage(bindings[i].id(),
							reinterpret_cast<void *>(node->address + off));
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
