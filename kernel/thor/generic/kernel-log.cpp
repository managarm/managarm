#include <frg/scope_exit.hpp>
#include <frg/string.hpp>
#include <frg/small_vector.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/int-call.hpp>
#include <thor-internal/kernel-io.hpp>
#include <thor-internal/kernel-log.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/reentrancy.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

namespace {
	// Protects pushes to the logRing.
	constinit ReentrancySafeSpinlock postMutex;

	constinit ReentrantRecordRing logRing;

	// Raised whenever new log records are published.
	constinit async::recurring_event logRingEvent;

	constinit SelfIntCall logWakeup{
		[] { logRingEvent.raise(); }
	};

	// True when we can use logWakeup (i.e., after self IPIs are available).
	constinit std::atomic<bool> logCallWakeup{false};
} // anonymous namespace

void postLogRecord(frg::string_view record) {
	{
		bool reentrant{postMutex.owner() == getCpuData()};
		if (!reentrant)
			postMutex.lock();
		frg::scope_exit unlockOnExit{[&] {
			if (!reentrant)
				postMutex.unlock();
		}};

		logRing.enqueue(record.data(), record.size());
	}

	if (logCallWakeup.load(std::memory_order_relaxed))
		logWakeup.schedule();
}

void enableLogWakeups() {
	logCallWakeup.store(true, std::memory_order_relaxed);
}

coroutine<void> waitForLog(uint64_t deqPtr) {
	co_await logRingEvent.async_wait_if([=] () -> bool {
		return logRing.peekHeadPtr() == deqPtr;
	});
}

frg::tuple<bool, uint64_t, uint64_t, size_t> retrieveLogRecord(uint64_t deqPtr, void *data, size_t maxSize) {
	return logRing.dequeueAt(deqPtr, data, maxSize);
}

//-----------------------------------------------------------------------------
// Kmsg implementation.
//-----------------------------------------------------------------------------

namespace {
	frg::manual_box<LogRingBuffer> globalKmsgRing;

	struct KmsgLogHandlerContext {
		uint64_t kmsgSeq_ = 0;
		int state_ = 0;

		frg::small_vector<char, logLineLength + 1, KernelAlloc> buffer_{*kernelAlloc};
	};

	void printChar(KmsgLogHandlerContext *ctx, char c) {
		auto emit = [&](const char c) {
			ctx->buffer_.push_back(c);

			if(c == '\n') {
				ctx->buffer_.push_back(0);
				getGlobalKmsgRing()->enqueue(ctx->buffer_.data(), ctx->buffer_.size());
				ctx->buffer_.resize(0);
			}
		};

		if(!ctx->state_) {
			if(c == '\x1B') {
				ctx->state_ = 1;
			} else {
				emit(c);
			}
		} else if(ctx->state_ == 1) {
			if(c == '[')
				ctx->state_ = 2;
			else {
				emit(c);
				ctx->state_ = 0;
			}
		} else if(ctx->state_ == 2) {
			if(c >= '0' && c <= '?') {
				// ignore them
			} else if(c >= ' ' && c <= '/') {
				ctx->state_ = 3;
			} else if(c >= '@' && c <= '~') {
				ctx->state_ = 0;
			} else {
				emit(c);
				ctx->state_ = 0;
			}
		} else if(ctx->state_ == 3) {
			if(c >= ' ' && c <= '/') {
				// ignore them
			} else if(c >= '@' && c <= '~') {
				ctx->state_ = 0;
			} else {
				emit(c);
				ctx->state_ = 0;
			}
		} else {
			assert(!"invalid state reached!");
			emit(c);
			ctx->state_ = 0;
		}
	}

	void setPriority(KmsgLogHandlerContext *ctx, Severity prio) {
		assert(ctx->buffer_.empty());
		auto now = getClockNanos() / 1000;
		frg::output_to(ctx->buffer_) << frg::fmt("{},{},{};", uint8_t(prio), ctx->kmsgSeq_++, now);
	}

	void translateRecord(KmsgLogHandlerContext *ctx, frg::string_view record) {
		auto [md, msg] = destructureLogRecord(record);
		setPriority(ctx, md.severity);
		for (size_t i = 0; i < msg.size(); ++i)
			printChar(ctx, msg[i]);
		printChar(ctx, '\n');
	}

	coroutine<void> dumpLogToKmsg() {
		char buffer[logLineLength];
		uint64_t deqPtr = 0;
		KmsgLogHandlerContext ctx;
		while (true) {
			auto [success, recordPtr, nextPtr, actualSize] = retrieveLogRecord(
					deqPtr, buffer, logLineLength);
			if (!success) {
				co_await waitForLog(deqPtr);
				continue;
			}

			frg::string_view record{buffer, actualSize};
			translateRecord(&ctx, record);

			deqPtr = nextPtr;
		}
	}

	initgraph::Task initKmsg{&globalInitEngine, "generic.init-kmsg",
		initgraph::Requires{getFibersAvailableStage(),
			getIoChannelsDiscoveredStage(), getTaskingAvailableStage()},
		[] {
			// Initialize globalKmsgRing and related functionality.
			void *kmsgMemory = kernelAlloc->allocate(1 << 20);
			globalKmsgRing.initialize(reinterpret_cast<uintptr_t>(kmsgMemory), 1 << 20);

			spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(), dumpLogToKmsg());

			// Expose globalKmsgRing as I/O channel.
			auto channel = solicitIoChannel("kernel-log");
			if(channel) {
				infoLogger() << "thor: Connecting logging to I/O channel" << frg::endlog;
				spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(),
						dumpRingToChannel(globalKmsgRing.get(), std::move(channel), 2048));
			}
		}
	};
} // namespace


LogRingBuffer *getGlobalKmsgRing() {
	return globalKmsgRing.get();
}

} // namespace thor
