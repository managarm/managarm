#include <thor-internal/ipl.hpp>
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
	auto irqLock = frg::guard(&irqMutex());
	Guard guard(lock);

	Handle handle = _nextHandle++;
	_descriptorMap.insert(handle, std::move(descriptor));
	return handle;
}

AnyDescriptor *Universe::getDescriptor(Guard &guard, Handle handle) {
	assert(guard.protects(&lock));

	return _descriptorMap.get(handle);
}

frg::optional<AnyDescriptor> Universe::detachDescriptor(Handle handle) {
	auto irqLock = frg::guard(&irqMutex());
	Guard guard(lock);

	return _descriptorMap.remove(handle);
}

} // namespace thor
