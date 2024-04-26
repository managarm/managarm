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

Handle Universe::attachDescriptor(Guard &guard, AnyDescriptor descriptor) {
	assert(guard.protects(&lock));

	Handle handle = _nextHandle++;
	_descriptorMap.insert(handle, std::move(descriptor));
	return handle;
}

AnyDescriptor *Universe::getDescriptor(Guard &guard, Handle handle) {
	assert(guard.protects(&lock));

	return _descriptorMap.get(handle);
}

frg::optional<AnyDescriptor> Universe::detachDescriptor(Guard &guard, Handle handle) {
	assert(guard.protects(&lock));

	return _descriptorMap.remove(handle);
}

} // namespace thor
