#include <async/recurring-event.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/kernel-log.hpp>
#include <thor-internal/arch/stack.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/main.hpp>

namespace thor {

namespace {
	// Protects the globalLogList.
	constinit IrqSpinlock listMutex;
} // anonymous namespace

constinit frg::intrusive_rcu_list<
	LogHandler,
	frg::locate_member<
		LogHandler,
		frg::intrusive_rcu_list_hook<LogHandler>,
		&LogHandler::hook
	>
> globalLogList;

void LogHandler::emitUrgent(frg::string_view record) {
	if (!takesUrgentLogs)
		panic();
	emit(record);
	flush();
}

void enableLogHandler(LogHandler *sink) {
	auto lock = frg::guard(&listMutex);
	globalLogList.push_back(sink);
}

void disableLogHandler(LogHandler *sink) {
	auto lock = frg::guard(&listMutex);
	globalLogList.erase(sink);
}

namespace {
	// This class splits long log messages into lines.
	// In also ensures that we never emit partial CSI sequences.
	class LogProcessor {
	public:
		void print(char c) {
			auto doesFit = [&] (int n) -> bool {
				return stagedLength + n < logLineLength;
			};

			auto emit = [&] (char c) {
				if (!stagedLength) {
					// Put log metadata in front of actual log message.
					LogMetadata md{.severity = severity};
					memcpy(stagingBuffer, &md, sizeof(LogMetadata));
					stagedLength = sizeof(LogMetadata);
				}

				assert(stagedLength < logLineLength);
				stagingBuffer[stagedLength++] = c;
			};

			auto flush = [&] () {
				if(!stagedLength)
					return;

				postLogRecord(frg::string_view{stagingBuffer, stagedLength}, expedited);

				// Reset our staging buffer.
				memset(stagingBuffer, 0, logLineLength);
				stagedLength = 0;
			};

			if(!csiState) {
				if(c == '\x1B') {
					csiState = 1;
				}else if(c == '\n') {
					flush();
				}else{
					if(!doesFit(1))
						flush();

					assert(doesFit(1));
					emit(c);
				}
			}else if(csiState == 1) {
				if(c == '[') {
					csiState = 2;
				}else{
					if(!doesFit(2))
						flush();

					assert(doesFit(2));
					emit('\x1B');
					emit(c);
					csiState = 0;
				}
			}else{
				// This is csiState == 2.
				if((c >= '0' && c <= '9') || (c == ';')) {
					if(csiLength < maximalCsiLength)
						csiBuffer[csiLength++] = c;
				}else{
					if(csiLength >= maximalCsiLength || !doesFit(3 + csiLength))
						flush();

					assert(doesFit(3 + csiLength));
					emit('\x1B');
					emit('[');
					for(int i = 0; i < csiLength; i++)
						emit(csiBuffer[i]);
					emit(c);
					csiState = 0;
					csiLength = 0;
				}
			}
		}

		void print(const char *str) {
			while(*str)
				print(*str++);
		}

		void setPriority(Severity prio) {
			severity = prio;
		}

		bool expedited{false};

	private:
		static constexpr int maximalCsiLength = 16;

		Severity severity{};

		char csiBuffer[maximalCsiLength]{};
		int csiState = 0;
		int csiLength = 0;

		char stagingBuffer[logLineLength]{};
		size_t stagedLength = 0;
	};
} // anonymous namespace

void panic() {
	disableInts();
	while(true)
		halt();
}

// We do not expect this to be called; however, libstdc++ contains references to abort().
extern "C" void abort() {
	panicLogger() << "abort() was called" << frg::endlog;
}

constinit frg::stack_buffer_logger<DebugSink, logLineLength> debugLogger;
constinit frg::stack_buffer_logger<WarningSink, logLineLength> warningLogger;
constinit frg::stack_buffer_logger<InfoSink, logLineLength> infoLogger;
constinit frg::stack_buffer_logger<UrgentSink, logLineLength> urgentLogger;
constinit frg::stack_buffer_logger<PanicSink, logLineLength> panicLogger;

void DebugSink::operator() (const char *msg) {
	LogProcessor logProcessor;
	logProcessor.setPriority(Severity::debug);
	logProcessor.print(msg);
	logProcessor.print('\n'); // Note: this is also required to flush.
}

void WarningSink::operator() (const char *msg) {
	LogProcessor logProcessor;
	logProcessor.setPriority(Severity::warning);
	logProcessor.print(msg);
	logProcessor.print('\n'); // Note: this is also required to flush.
}

void InfoSink::operator() (const char *msg) {
	LogProcessor logProcessor;
	logProcessor.setPriority(Severity::info);
	logProcessor.print(msg);
	logProcessor.print('\n'); // Note: this is also required to flush.
}

void UrgentSink::operator() (const char *msg) {
	LogProcessor logProcessor;
	logProcessor.expedited = true;
	logProcessor.setPriority(Severity::critical);
	logProcessor.print(msg);
	logProcessor.print('\n'); // Note: this is also required to flush.
}

void PanicSink::operator() (const char *msg) {
	LogProcessor logProcessor;
	logProcessor.expedited = true;
	logProcessor.setPriority(Severity::emergency);
	logProcessor.print(msg);
	logProcessor.print('\n'); // Note: this is also required to flush.
}

void PanicSink::finalize(bool) {
	RobustIrqLock irqLock;

#ifdef THOR_HAS_FRAME_POINTERS
	urgentLogger() << "Stacktrace:" << frg::endlog;
	walkThisStack([&](uintptr_t ip) {
		urgentLogger() << "\t<" << (void*)ip << ">" << frg::endlog;
	});
#endif

	panic();
}

} // namespace thor

extern "C" [[gnu::noreturn]] void __assert_fail(const char *assertion, const char *file,
		unsigned int line, const char *function) {
	thor::panicLogger() << "Assertion failed: " << assertion << "\n"
			<< "In function " << function
			<< " at " << file << ":" << line << frg::endlog;
	__builtin_trap();
}

// This is required for virtual destructors. It should not be called though.
void operator delete(void *, size_t) {
	thor::panicLogger() << "thor: operator delete() called" << frg::endlog;
}

extern "C" void __cxa_pure_virtual() {
	thor::panicLogger() << "Pure virtual call" << frg::endlog;
}
