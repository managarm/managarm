#include <frg/bitops.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/ipl.hpp>

namespace thor {

void panicOnIllegalIplEntry(Ipl newIpl, Ipl currentIpl) {
	panicLogger() << "thor: Cannot enter IPL " << newIpl << " context from IPL " << currentIpl << frg::endlog;
	__builtin_trap();
}

// Run handlers scheduled by deferToIplLowerThan(L) where: current < L <= ceiling.
// Precondition: no calls are currently scheduled by deferToIplLowerThan(L') where L' > ceiling.
void handleIplDeferred(Ipl current, Ipl ceiling) {
	auto cpuData = getCpuData();
	while (true) {
		// Note: we need to re-read iplDeferred in each iteration as new bits may have been set.
		auto deferred = cpuData->iplDeferred.load(std::memory_order_relaxed);
		if (!deferred)
			break;

		Ipl l = 8 * sizeof(IplMask) - frg::clz(deferred);
		assert(l <= ceiling);
		if (l <= current)
			break;

		cpuData->iplDeferred.fetch_and(
			~(static_cast<IplMask>(1) << (l - 1)),
			std::memory_order_relaxed
		);
		// Invariant: no handler for level L will call deferToIplLowerThan(L') where L' >= L.
		ceiling = l;

		// Note: during this switch, the currentIpl() is not necessarily l.
		// If handlers rely on running at a certain IPL, they need to raise it.
		switch (l) {
			case ipl::schedule:
				// TODO: Instead of sending a ping IPI, we can also schedule here.
				sendPingIpi(cpuData);
				break;
			default:
				// Nothing to do.
		}
	}
}

} // namespace thor
