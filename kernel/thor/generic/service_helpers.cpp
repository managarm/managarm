
#include <thor-internal/service_helpers.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/arch/hpet.hpp>

namespace thor {

void fiberCopyToBundle(MemoryView *bundle, ptrdiff_t offset, const void *pointer, size_t size) {
	struct Closure {
		static void copied(CopyToBundleNode *base) {
			auto closure = frg::container_of(base, &Closure::copy);
			KernelFiber::unblockOther(&closure->blocker);
		}

		FiberBlocker blocker;
		CopyToBundleNode copy;
	} closure;

	closure.blocker.setup();
	if(!copyToBundle(bundle, offset, pointer, size, &closure.copy, &Closure::copied))
		KernelFiber::blockCurrent(&closure.blocker);
}

void fiberCopyFromBundle(MemoryView *bundle, ptrdiff_t offset, void *pointer, size_t size) {
	struct Closure {
		static void copied(CopyFromBundleNode *base) {
			auto closure = frg::container_of(base, &Closure::copy);
			KernelFiber::unblockOther(&closure->blocker);
		}

		FiberBlocker blocker;
		CopyFromBundleNode copy;
	} closure;

	closure.blocker.setup();
	if(!copyFromBundle(bundle, offset, pointer, size, &closure.copy, &Closure::copied))
		KernelFiber::blockCurrent(&closure.blocker);
}

} // namespace thor

