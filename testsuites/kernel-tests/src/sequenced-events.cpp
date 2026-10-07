#include <cassert>

#include <async/algorithm.hpp>
#include <async/result.hpp>
#include <hel.h>
#include <hel-syscalls.h>
#include <helix/ipc.hpp>

#include "testsuite.hpp"

namespace {

struct SequencedEvent {
	SequencedEvent() {
		HelHandle rawWaitHandle;
		HelHandle rawRaiseHandle;
		HEL_CHECK(helCreateSequencedEvent(&rawWaitHandle, &rawRaiseHandle));
		waitHandle = helix::UniqueDescriptor{rawWaitHandle};
		raiseHandle = helix::UniqueDescriptor{rawRaiseHandle};
	}

	helix::UniqueDescriptor waitHandle;
	helix::UniqueDescriptor raiseHandle;
};

async::result<void> testSequencedEventRaise() {
	SequencedEvent event;

	HEL_CHECK(helRaiseEvent(event.raiseHandle.getHandle()));
	HEL_CHECK(helRaiseEvent(event.raiseHandle.getHandle()));

	// Raises that happened before the wait are coalesced.
	auto result = co_await helix_ng::awaitEvent(event.waitHandle, 0);
	assert(result.error() == kHelErrNone);
	assert(result.sequence() == 2);

	result = co_await helix_ng::awaitEvent(event.waitHandle, 1);
	assert(result.error() == kHelErrNone);
	assert(result.sequence() == 2);

	// Sequence numbers that were not reached yet cannot be waited for.
	result = co_await helix_ng::awaitEvent(event.waitHandle, 3);
	assert(result.error() == kHelErrIllegalArgs);
}

async::result<void> testSequencedEventWakeup() {
	SequencedEvent event;

	co_await async::race_and_cancel(
		async::lambda([&](async::cancellation_token ct) -> async::result<void> {
			auto result = co_await helix_ng::awaitEvent(event.waitHandle, 0, ct);
			assert(result.error() == kHelErrNone);
			assert(result.sequence() == 1);
		}),
		async::lambda([&](async::cancellation_token) -> async::result<void> {
			// The kernel processes the nop after the wait, so the wait is pending once it completes.
			co_await helix_ng::asyncNop();
			// The raise completes the wait before the cancellation arrives.
			HEL_CHECK(helRaiseEvent(event.raiseHandle.getHandle()));
		})
	);
}

async::result<void> testSequencedEventCancellation() {
	SequencedEvent event;

	co_await async::race_and_cancel(
		async::lambda([&](async::cancellation_token ct) -> async::result<void> {
			auto result = co_await helix_ng::awaitEvent(event.waitHandle, 0, ct);
			assert(result.error() == kHelErrCancelled);
		}),
		[](async::cancellation_token) -> async::result<void> {
			// The kernel processes the nop after the wait, so the wait is pending once it completes.
			co_await helix_ng::asyncNop();
		}
	);

	// The event is still usable after the cancellation.
	HEL_CHECK(helRaiseEvent(event.raiseHandle.getHandle()));
	auto result = co_await helix_ng::awaitEvent(event.waitHandle, 0);
	assert(result.error() == kHelErrNone);
	assert(result.sequence() == 1);
}

async::result<void> testSequencedEventRaiseEndGone() {
	SequencedEvent event;

	HEL_CHECK(helRaiseEvent(event.raiseHandle.getHandle()));

	auto result = co_await helix_ng::awaitEvent(event.waitHandle, 0);
	assert(result.error() == kHelErrNone);
	assert(result.sequence() == 1);

	co_await async::race_and_cancel(
		async::lambda([&](async::cancellation_token ct) -> async::result<void> {
			auto result = co_await helix_ng::awaitEvent(event.waitHandle, 1, ct);
			assert(result.error() == kHelErrEndOfLane);
		}),
		async::lambda([&](async::cancellation_token) -> async::result<void> {
			// The kernel processes the nop after the wait, so the wait is pending once it completes.
			co_await helix_ng::asyncNop();
			// Closing the raise handle completes the wait before the cancellation arrives.
			event.raiseHandle = helix::UniqueDescriptor{};
		})
	);

	// Raises that happened before the raise end went away are still reported.
	result = co_await helix_ng::awaitEvent(event.waitHandle, 0);
	assert(result.error() == kHelErrNone);
	assert(result.sequence() == 1);

	result = co_await helix_ng::awaitEvent(event.waitHandle, 1);
	assert(result.error() == kHelErrEndOfLane);
}

} // anonymous namespace

DEFINE_TEST(sequencedEventRaise, ([] {
	async::run(testSequencedEventRaise(), helix::currentDispatcher);
}))

DEFINE_TEST(sequencedEventWakeup, ([] {
	async::run(testSequencedEventWakeup(), helix::currentDispatcher);
}))

DEFINE_TEST(sequencedEventCancellation, ([] {
	async::run(testSequencedEventCancellation(), helix::currentDispatcher);
}))

DEFINE_TEST(sequencedEventRaiseEndGone, ([] {
	async::run(testSequencedEventRaiseEndGone(), helix::currentDispatcher);
}))

DEFINE_TEST(sequencedEventWaitEndGone, ([] {
	SequencedEvent event;

	HEL_CHECK(helRaiseEvent(event.raiseHandle.getHandle()));
	event.waitHandle = helix::UniqueDescriptor{};
	assert(helRaiseEvent(event.raiseHandle.getHandle()) == kHelErrEndOfLane);
}))

DEFINE_TEST(sequencedEventWrongEnd, ([] {
	SequencedEvent event;

	assert(helRaiseEvent(event.waitHandle.getHandle()) == kHelErrBadRights);
}))
