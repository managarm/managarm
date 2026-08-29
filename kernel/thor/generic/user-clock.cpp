#include <hel.h>
#include <thor-internal/arch-generic/timer.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/memory-view.hpp>
#include <thor-internal/user-clock.hpp>

namespace thor {

namespace {

smarter::shared_ptr<ImmediateMemory> &userClockMemory() {
	static frg::eternal<smarter::shared_ptr<ImmediateMemory>> singleton = [] {
		auto memoryOutcome = ImmediateMemory::create(kPageSize);
		if(!memoryOutcome)
			panicLogger() << "thor: Failed to allocate the clock page" << frg::endlog;
		return std::move(*memoryOutcome);
	}();
	return singleton.get();
}

uint32_t translateClockType(UserspaceClockType type) {
	switch(type) {
		case UserspaceClockType::x86Tsc: return kHelClockTsc;
		case UserspaceClockType::armCntpct: return kHelClockCntpct;
		case UserspaceClockType::armCntvct: return kHelClockCntvct;
		case UserspaceClockType::riscvTime: return kHelClockTime;
		default: return kHelClockNone;
	}
}

initgraph::Task publishClockPageTask{&globalInitEngine, "generic.publish-clock-page",
	initgraph::Requires{getTaskingAvailableStage()},
	[] {
		// Instantiate the memory object even if there is nothing to publish:
		// helObtainHandle() hands it out unconditionally and cannot construct it concurrently.
		auto page = userClockMemory()->accessImmediate<HelClockPage>(0);

		auto clock = getUserspaceClock();
		if(clock.type == UserspaceClockType::none) {
			infoLogger() << "thor: Monotonic clock cannot be read from userspace"
					<< frg::endlog;
			return;
		}

		// Start the seqlock write.
		// Only the kernel has write access, so we can trust the previous seqlock value.
		auto seqlock = __atomic_load_n(&page->seqlock, __ATOMIC_RELAXED);
		assert(!(seqlock & 1));
		__atomic_store_n(&page->seqlock, seqlock + 1, __ATOMIC_RELAXED);
		__atomic_thread_fence(__ATOMIC_RELEASE);

		// Perform the actual update.
		__atomic_store_n(&page->clockType, translateClockType(clock.type), __ATOMIC_RELAXED);
		__atomic_store_n(&page->tickShift, clock.tickDuration.s, __ATOMIC_RELAXED);
		__atomic_store_n(&page->tickFactor, clock.tickDuration.f, __ATOMIC_RELAXED);

		// Complete the seqlock write.
		__atomic_store_n(&page->seqlock, seqlock + 2, __ATOMIC_RELEASE);
	}
};

} // anonymous namespace

smarter::shared_ptr<MemoryView> getUserClockMemory() {
	return userClockMemory();
}

} // namespace thor
