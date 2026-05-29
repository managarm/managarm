#include <thor-internal/ipl.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/universe.hpp>

namespace thor {

namespace {
	constexpr bool logCleanup = false;
}

Universe::Universe()
: _descriptorMap{frg::hash<Handle>{}, *kernelAlloc}, _nextHandle{1} { }

Universe::~Universe() {
	if(logCleanup)
		debugLogger() << "thor: Universe is deallocated" << frg::endlog;
}

Handle Universe::attachDescriptor(AnyDescriptor descriptor) {
	PreemptionGuard preemptionGuard;
	AdaptiveMutex::Guard lock{_mutex};

	Handle handle = _nextHandle++;
	_descriptorMap.insert(handle, std::move(descriptor));
	return handle;
}

std::optional<AnyDescriptor> Universe::getDescriptor(Handle handle) {
	PreemptionGuard preemptionGuard;
	AdaptiveMutex::Guard lock{_mutex};

	auto *desc = _descriptorMap.get(handle);
	if(!desc)
		return std::nullopt;
	return *desc;
}

frg::optional<AnyDescriptor> Universe::detachDescriptor(Handle handle) {
	PreemptionGuard preemptionGuard;
	AdaptiveMutex::Guard lock{_mutex};

	return _descriptorMap.remove(handle);
}

} // namespace thor
