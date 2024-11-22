#pragma once

#include <thor-internal/debug.hpp>
#include <thor-internal/int-call.hpp>
#include <thor-internal/ring-buffer.hpp>

namespace thor {

struct GlobalLogRing {
	void enable();

	auto wait(uint64_t deqPtr) {
		return event_.async_wait_if([=, this] () -> bool {
			return ring_.peekHeadPtr() == deqPtr;
		});
	}

	frg::tuple<bool, uint64_t, uint64_t, size_t>
	dequeueAt(uint64_t deqPtr, void *data, size_t maxSize) {
		return ring_.dequeueAt(deqPtr, data, maxSize);
	}

private:
	struct Wakeup {
		Wakeup(GlobalLogRing *ptr);

		void operator() ();

	private:
		GlobalLogRing *ptr_;
	};

	struct Handler : LogHandler {
		Handler(GlobalLogRing *ptr);

		void emit(frg::string_view record) override;

	private:
		GlobalLogRing *ptr_;
	};

	async::recurring_event event_;
	SingleContextRecordRing ring_;
	SelfIntCall<Wakeup> wakeup_{this};
	Handler handler_{this};
};

void initializeLog();

GlobalLogRing *getGlobalLogRing();

LogRingBuffer *getGlobalKmsgRing();

} // namespace thor
