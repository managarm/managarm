
#include <thor-internal/fiber.hpp>
#include <thor-internal/kernel-io.hpp>
#include <thor-internal/kernel-log.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/timer.hpp>
#include <frg/string.hpp>

namespace thor {

namespace {
	struct KmsgLogHandler : public LogHandler {
		void printChar(char c) override {
			buffer_.push_back(c);

			if(c == '\n') {
				buffer_.push_back(0);
				getGlobalLogRing()->enqueue(buffer_.data(), buffer_.size());
				buffer_.resize(0);
			}
		}

		void setPriority(Severity prio) override {
			assert(buffer_.empty());
			auto now = systemClockSource()->currentNanos() / 1000;
			frg::output_to(buffer_) << frg::fmt("{},{},{};", uint8_t(prio), kmsgSeq_++, now);
		}

		void resetPriority() override {
			// no-op
		}

	private:
		uint64_t kmsgSeq_;
		frg::vector<char, KernelAlloc> buffer_{*kernelAlloc};
	};

	frg::manual_box<LogRingBuffer> globalLogRing;

	initgraph::Task initLogSinks{&globalInitEngine, "generic.init-kernel-log",
		initgraph::Requires{getFibersAvailableStage(),
			getIoChannelsDiscoveredStage(), getTaskingAvailableStage()},
		[] {
			initializeLog();

			auto channel = solicitIoChannel("kernel-log");
			if(channel) {
				infoLogger() << "thor: Connecting logging to I/O channel" << frg::endlog;
				async::detach_with_allocator(*kernelAlloc,
						dumpRingToChannel(globalLogRing.get(), std::move(channel), 2048));
			}
		}
	};
}

frg::manual_box<KmsgLogHandler> kmsgLogHandler;

void initializeLog() {
	void *logMemory = kernelAlloc->allocate(1 << 20);
	globalLogRing.initialize(reinterpret_cast<uintptr_t>(logMemory), 1 << 20);
	kmsgLogHandler.initialize();
	enableLogHandler(kmsgLogHandler.get());
}

LogRingBuffer *getGlobalLogRing() {
	return globalLogRing.get();
}

} // namespace thor
