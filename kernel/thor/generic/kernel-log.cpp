
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
			buffer_[offset_++] = c;

			if(c == '\n') {
				getGlobalLogRing()->enqueue(buffer_, offset_);
				offset_ = 0;
			}
		}
	private:
		char buffer_[2048] = {};
		size_t offset_ = 0;
	};

	frg::manual_box<LogRingBuffer> globalLogRing;

	initgraph::Task initLogSinks{&globalInitEngine, "generic.init-kernel-log",
		initgraph::Requires{getFibersAvailableStage(),
			getIoChannelsDiscoveredStage()},
		[] {
			auto channel = solicitIoChannel("kernel-log");
			if(channel) {
				infoLogger() << "thor: Connecting logging to I/O channel" << frg::endlog;
				async::detach_with_allocator(*kernelAlloc,
						dumpRingToChannel(globalLogRing.get(), std::move(channel), 2048));
			}
		}
	};
}

constinit KmsgLogHandler kmsgLogHandler;

void initializeLog() {
	void *logMemory = kernelAlloc->allocate(1 << 20);
	globalLogRing.initialize(reinterpret_cast<uintptr_t>(logMemory), 1 << 20);
	enableLogHandler(&kmsgLogHandler);
}

LogRingBuffer *getGlobalLogRing() {
	return globalLogRing.get();
}

} // namespace thor
