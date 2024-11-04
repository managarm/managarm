
#include <frg/string.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kernel-io.hpp>
#include <thor-internal/kernel-log.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

namespace {
struct KmsgLogHandlerContext {
	uint64_t kmsgSeq_ = 0;
	int state_ = 0;

	frg::vector<char, KernelAlloc> buffer_{*kernelAlloc};
};

struct KmsgLogHandler : public LogHandler {
	KmsgLogHandler(KmsgLogHandlerContext *context) : LogHandler(context) {}

	void printChar(char c) override {
		auto emit = [&](const char c) {
			ctx()->buffer_.push_back(c);

			if (c == '\n') {
				ctx()->buffer_.push_back(0);
				getGlobalLogRing()->enqueue(ctx()->buffer_.data(), ctx()->buffer_.size());
				ctx()->buffer_.resize(0);
			}
		};

		if (!ctx()->state_) {
			if (c == '\x1B') {
				ctx()->state_ = 1;
			} else {
				emit(c);
			}
		} else if (ctx()->state_ == 1) {
			if (c == '[')
				ctx()->state_ = 2;
			else {
				emit(c);
				ctx()->state_ = 0;
			}
		} else if (ctx()->state_ == 2) {
			if (c >= '0' && c <= '?') {
				// ignore them
			} else if (c >= ' ' && c <= '/') {
				ctx()->state_ = 3;
			} else if (c >= '@' && c <= '~') {
				ctx()->state_ = 0;
			} else {
				emit(c);
				ctx()->state_ = 0;
			}
		} else if (ctx()->state_ == 3) {
			if (c >= ' ' && c <= '/') {
				// ignore them
			} else if (c >= '@' && c <= '~') {
				ctx()->state_ = 0;
			} else {
				emit(c);
				ctx()->state_ = 0;
			}
		} else {
			assert(!"invalid state reached!");
			emit(c);
			ctx()->state_ = 0;
		}
	}

	void setPriority(Severity prio) override {
		assert(ctx()->buffer_.empty());
		auto now = systemClockSource()->currentNanos() / 1000;
		frg::output_to(ctx()->buffer_)
		    << frg::fmt("{},{},{};", uint8_t(prio), ctx()->kmsgSeq_++, now);
	}

	void resetPriority() override {
		// no-op
	}

	KmsgLogHandlerContext *ctx() { return reinterpret_cast<KmsgLogHandlerContext *>(context); }
};

frg::manual_box<LogRingBuffer> globalLogRing;

initgraph::Task initLogSinks{
    &globalInitEngine,
    "generic.init-kernel-log",
    initgraph::Requires{
        getFibersAvailableStage(), getIoChannelsDiscoveredStage(), getTaskingAvailableStage()
    },
    [] {
	    initializeLog();

	    auto channel = solicitIoChannel("kernel-log");
	    if (channel) {
		    infoLogger() << "thor: Connecting logging to I/O channel" << frg::endlog;
		    async::detach_with_allocator(
		        *kernelAlloc, dumpRingToChannel(globalLogRing.get(), std::move(channel), 2048)
		    );
	    }
    }
};
} // namespace

frg::manual_box<KmsgLogHandlerContext> kmsgLogHandlerContext;
frg::manual_box<KmsgLogHandler> kmsgLogHandler;

void initializeLog() {
	void *logMemory = kernelAlloc->allocate(1 << 20);
	globalLogRing.initialize(reinterpret_cast<uintptr_t>(logMemory), 1 << 20);
	kmsgLogHandlerContext.initialize();
	kmsgLogHandler.initialize(kmsgLogHandlerContext.get());
	enableLogHandler(kmsgLogHandler.get());
}

LogRingBuffer *getGlobalLogRing() { return globalLogRing.get(); }

} // namespace thor
