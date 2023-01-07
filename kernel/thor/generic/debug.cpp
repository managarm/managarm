#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/arch/stack.hpp>

namespace thor {

namespace {
	// Protects the data structures below.
	constinit frg::ticket_spinlock logMutex;

	constinit LogMessage logQueue[1024]{};
	constinit size_t logHead = 0;

	frg::manual_box<frg::intrusive_list<
		LogHandler,
		frg::locate_member<
			LogHandler,
			frg::default_list_hook<LogHandler>,
			&LogHandler::hook
		>
	>> globalLogList;
} // anonymous namespace

size_t currentLogSequence() {
	return logHead;
}

void copyLogMessage(size_t sequence, LogMessage &msg) {
	memcpy(msg.text, logQueue[sequence % 1024].text, logLineLength);
}

void enableLogHandler(LogHandler *sink) {
	if (!globalLogList)
		globalLogList.initialize();

	globalLogList->push_back(sink);
}

void disableLogHandler(LogHandler *sink) {
	if (!globalLogList)
		globalLogList.initialize();

	auto it = globalLogList->iterator_to(sink);
	globalLogList->erase(it);
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
				stagingBuffer[stagedLength++] = c;

				for (const auto &it : *globalLogList)
					it->printChar(c);
			};

			auto flush = [&] () {
				// Copy to the log queue.
				memcpy(logQueue[logHead % 1024].text, stagingBuffer, logLineLength);
				logHead++;
				// Reset our staging buffer.
				memset(stagingBuffer, 0, logLineLength);
				stagedLength = 0;

				for (const auto &it : *globalLogList)
					it->printChar('\n');
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
						csiBuffer[csiLength] = c;
					csiLength++;
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

	private:
		static constexpr int maximalCsiLength = 16;

		char csiBuffer[maximalCsiLength]{};
		int csiState = 0;
		int csiLength = 0;

		char stagingBuffer[logLineLength]{};
		size_t stagedLength = 0;
	};

	constinit LogProcessor logProcessor;
} // anonymous namespace

void panic() {
	disableInts();
	while(true)
		halt();
}

constinit frg::stack_buffer_logger<InfoSink, logLineLength> infoLogger;
constinit frg::stack_buffer_logger<UrgentSink, logLineLength> urgentLogger;
constinit frg::stack_buffer_logger<PanicSink, logLineLength> panicLogger;

void InfoSink::operator() (const char *msg) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&logMutex);

	logProcessor.print(msg);
	logProcessor.print('\n');
}

void UrgentSink::operator() (const char *msg) {
	StatelessIrqLock irqLock;
	auto lock = frg::guard(&logMutex);

	logProcessor.print(msg);
	logProcessor.print('\n');
}

void PanicSink::operator() (const char *msg) {
	StatelessIrqLock irqLock;

	auto lock = frg::guard(&logMutex);

	logProcessor.print(msg);
	logProcessor.print('\n');
}

void PanicSink::finalize(bool) {
	StatelessIrqLock irqLock;

#ifdef THOR_HAS_FRAME_POINTERS
	urgentLogger() << "Stacktrace:" << frg::endlog;
	walkThisStack([&](uintptr_t ip) {
		urgentLogger() << "\t<" << (void*)ip << ">" << frg::endlog;
	});
#endif

	panic();
}

} // namespace thor

extern "C" void __assert_fail(const char *assertion, const char *file,
		unsigned int line, const char *function) {
	thor::panicLogger() << "Assertion failed: " << assertion << "\n"
			<< "In function " << function
			<< " at " << file << ":" << line << frg::endlog;
}

extern "C" void __cxa_pure_virtual() {
	thor::panicLogger() << "Pure virtual call" << frg::endlog;
}
