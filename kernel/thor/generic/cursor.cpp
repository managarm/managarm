#include <thor-internal/arch-generic/cursor.hpp>
#include <thor-internal/cpu-data.hpp>

namespace thor::detail {

void runWithLockedSpace(PageSpace *space, void (*fn)(void *), void *ctxt) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&space->tableMutex());
	{
		fn(ctxt);
	}
}

} // namespace thor::detail
