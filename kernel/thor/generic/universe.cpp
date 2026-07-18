#include <thor-internal/address-space.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/stream.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/universe.hpp>

namespace thor {

namespace {
	constexpr bool logCleanup = false;
}

template<>
AnyDescriptor AnyDescriptor::make<DescriptorType::thread>(
		smarter::shared_ptr<Thread, ActiveHandle> ptr) {
	assert(ptr);

	AnyDescriptor descriptor;
	descriptor.type_ = DescriptorType::thread;
	descriptor.object_ = ptr.get();
	descriptor.ctr_ = &ptr->_activeCtr;
	ptr.release();
	return descriptor;
}

template<>
AnyDescriptor AnyDescriptor::make<DescriptorType::addressSpace>(
		smarter::shared_ptr<AddressSpace, BindableHandle> ptr) {
	assert(ptr);

	AnyDescriptor descriptor;
	descriptor.type_ = DescriptorType::addressSpace;
	descriptor.object_ = ptr.get();
	descriptor.ctr_ = &ptr->_bindableCtr;
	ptr.release();
	return descriptor;
}

template<>
AnyDescriptor AnyDescriptor::make<DescriptorType::lane>(
		smarter::shared_ptr<Stream, LanePolicy> ptr) {
	assert(ptr);

	AnyDescriptor descriptor;
	descriptor.type_ = DescriptorType::lane;
	auto stream = ptr.get();
	descriptor.extra_ = static_cast<uint8_t>(laneOf(ptr));
	descriptor.object_ = stream;
	descriptor.ctr_ = &stream->peerCounter(static_cast<int>(descriptor.extra_));
	ptr.release();
	return descriptor;
}

template<>
std::expected<smarter::shared_ptr<Stream, LanePolicy>, Error>
AnyDescriptor::resolveObject<DescriptorType::lane>() const {
	if(type_ != DescriptorType::lane)
		return std::unexpected{Error::badDescriptor};

	auto stream = static_cast<Stream *>(object_);
	auto lane = static_cast<int>(extra_);
	stream->peerCounter(lane).increment();
	return adoptLane(stream->selfPtr, lane);
}

void AnyDescriptor::releaseOnZero_() {
	switch(type_) {
	case DescriptorType::thread: {
		auto thread = static_cast<Thread *>(object_);
		thread->dispose();
		thread->self.policy().decrement();
		break;
	}
	case DescriptorType::addressSpace: {
		auto space = static_cast<AddressSpace *>(object_);
		space->dispose();
		space->selfPtr.policy().decrement();
		break;
	}
	case DescriptorType::lane: {
		auto stream = static_cast<Stream *>(object_);
		Stream::onPeersZero(stream, static_cast<int>(extra_));
		stream->selfPtr.policy().decrement();
		break;
	}
	default: {
		// All remaining descriptor types hold a reference through the meta object's counter.
		smarter::meta_object_base::from_ctr(ctr_)->finalize();
		break;
	}
	}
}

Universe::Universe()
: _descriptorMap{frg::hash<Handle>{}, *kernelAlloc}, _nextHandle{1} { }

Universe::~Universe() {
	if(logCleanup)
		debugLogger() << "thor: Universe is deallocated" << frg::endlog;
}

Handle Universe::attachDescriptor(AnyDescriptor descriptor) {
	assert(descriptor.type() != DescriptorType::none);

	auto irqLock = frg::guard(&irqMutex());
	Guard guard(lock);

	Handle handle = _nextHandle++;
	_descriptorMap.insert(handle, std::move(descriptor));
	return handle;
}

std::optional<AnyDescriptor> Universe::getDescriptor(Handle handle) {
	auto irqLock = frg::guard(&irqMutex());
	Guard guard(lock);

	auto *desc = _descriptorMap.get(handle);
	if(!desc)
		return std::nullopt;
	return *desc;
}

frg::optional<AnyDescriptor> Universe::detachDescriptor(Handle handle) {
	auto irqLock = frg::guard(&irqMutex());
	Guard guard(lock);

	return _descriptorMap.remove(handle);
}

} // namespace thor
