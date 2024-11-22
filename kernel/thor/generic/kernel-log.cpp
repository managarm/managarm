#include <thor-internal/fiber.hpp>
#include <thor-internal/kernel-io.hpp>
#include <thor-internal/kernel-log.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/timer.hpp>
#include <frg/string.hpp>
#include <frg/small_vector.hpp>

namespace thor {

//-----------------------------------------------------------------------------
// GlobalLogRing implementation.
//-----------------------------------------------------------------------------

namespace {

constinit GlobalLogRing *globalLogRing{nullptr};

} // namespace

void GlobalLogRing::enable() {
	enableLogHandler(&handler_);
}

// GlobalLogRing::Wakeup implementation.

GlobalLogRing::Wakeup::Wakeup(GlobalLogRing *ptr)
: ptr_{ptr} {}

void GlobalLogRing::Wakeup::operator() () {
	ptr_->event_.raise();
}

// GlobalLogRing::Handler implementation.

GlobalLogRing::Handler::Handler(GlobalLogRing *ptr)
: ptr_{ptr} {}

void GlobalLogRing::Handler::emit(frg::string_view record) {
	ptr_->ring_.enqueue(record.data(), record.size());
	ptr_->wakeup_.schedule();
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
		auto now = systemClockSource()->currentNanos() / 1000;
		frg::output_to(ctx->buffer_) << frg::fmt("{},{},{};", uint8_t(prio), ctx->kmsgSeq_++, now);
	}

	void translateRecord(KmsgLogHandlerContext *ctx, frg::string_view record) {
		auto [md, msg] = destructureLogRecord(record);
		setPriority(ctx, md.severity);
		for (size_t i = 0; i < msg.size(); ++i)
			printChar(ctx, msg[i]);
		printChar(ctx, '\n');
	}

	initgraph::Task initLogSinks{&globalInitEngine, "generic.init-kernel-log",
		initgraph::Requires{getFibersAvailableStage(),
			getIoChannelsDiscoveredStage(), getTaskingAvailableStage()},
		[] {
			initializeLog();

			auto channel = solicitIoChannel("kernel-log");
			if(channel) {
				infoLogger() << "thor: Connecting logging to I/O channel" << frg::endlog;
				async::detach_with_allocator(*kernelAlloc,
						dumpRingToChannel(globalKmsgRing.get(), std::move(channel), 2048));
			}
		}
	};
} // namespace


namespace {

coroutine<void> dumpLogToKmsg() {
	auto glr = getGlobalLogRing();
	char buffer[logLineLength];
	uint64_t deqPtr = 0;
	KmsgLogHandlerContext ctx;
	while (true) {
		auto [success, recordPtr, nextPtr, actualSize] = glr->dequeueAt(
				deqPtr, buffer, logLineLength);
		if (!success) {
			co_await glr->wait(deqPtr);
			continue;
		}

		frg::string_view record{buffer, actualSize};
		translateRecord(&ctx, record);

		deqPtr = nextPtr;
	}
}

} // namespace

void initializeLog() {
	// Initialize globalLogRing.
	globalLogRing = frg::construct<GlobalLogRing>(*kernelAlloc);
	globalLogRing->enable();

	// Initialize globalKmsgRing and related functionality.
	void *logMemory = kernelAlloc->allocate(1 << 20);
	globalKmsgRing.initialize(reinterpret_cast<uintptr_t>(logMemory), 1 << 20);

	async::detach_with_allocator(*kernelAlloc, dumpLogToKmsg());
}

GlobalLogRing *getGlobalLogRing() {
	return globalLogRing;
}

LogRingBuffer *getGlobalKmsgRing() {
	return globalKmsgRing.get();
}

} // namespace thor
