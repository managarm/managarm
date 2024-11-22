#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/arch/stack.hpp>
#include <thor-internal/ring-buffer.hpp>

namespace thor {

namespace {
	// Protects the data structures below.
	constinit frg::ticket_spinlock logMutex;

	frg::manual_box<frg::intrusive_list<
		LogHandler,
		frg::locate_member<
			LogHandler,
			frg::default_list_hook<LogHandler>,
			&LogHandler::hook
		>
	>> globalLogList;
} // anonymous namespace

void LogHandler::emitUrgent(frg::string_view record) {
	if (!takesUrgentLogs)
		panic();
	emit(record);
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
	bool checkEmitting();
	bool tryStartEmitting();
	bool tryFinishEmitting();

	// Assumption: !intsAreEnabled().
	void emitLogsFromRing() {
		auto *cpuData = getCpuData();

		// Only start emitting logs if we are not a reentrant context.
		if (!tryStartEmitting())
			return;

		do {
			while (true) {
				auto lock = frg::guard(&logMutex);

				char buffer[logLineLength];
				auto [success, recordPtr, nextPtr, actualSize] = cpuData->localLogRing->dequeueAt(
						cpuData->localLogSeq, buffer, logLineLength);
				if (!success)
					break;

				if (actualSize < sizeof(LogMetadata))
					panic();
				frg::string_view record{buffer, actualSize};
				for (const auto &it : *globalLogList)
					it->emit(record);

				cpuData->localLogSeq = nextPtr;
			}

			// Emit logs until no reentrant context has set the RS_PENDING flag.
		} while(!tryFinishEmitting());
	}

	// Assumption: !intsAreEnabled().
	bool checkEmitting() {
		auto s = getCpuData()->reentrantLogState.load(std::memory_order_relaxed);
		return s & CpuData::RS_EMITTING;
	}

	// Assumption: !intsAreEnabled().
	bool tryStartEmitting() {
		auto *cpuData = getCpuData();
		unsigned int s = cpuData->reentrantLogState.load(std::memory_order_relaxed);
		while (true) {
			if (s) {
				bool cas = cpuData->reentrantLogState.compare_exchange_weak(
					s, s | CpuData::RS_PENDING, std::memory_order_relaxed
				);
				if (cas)
					return false;
			} else {
				bool cas = cpuData->reentrantLogState.compare_exchange_weak(
					s, CpuData::RS_EMITTING, std::memory_order_relaxed
				);
				if (cas)
					return true;
			}
		}
	}

	// Assumption: !intsAreEnabled().
	bool tryFinishEmitting() {
		auto *cpuData = getCpuData();
		unsigned int s = cpuData->reentrantLogState.load(std::memory_order_relaxed);
		while (true) {
			if (!(s & CpuData::RS_EMITTING))
				__builtin_trap();
			if (s & CpuData::RS_PENDING) {
				bool cas = cpuData->reentrantLogState.compare_exchange_weak(
					s, s & ~CpuData::RS_PENDING, std::memory_order_relaxed
				);
				if (cas)
					return false;
			} else {
				bool cas = cpuData->reentrantLogState.compare_exchange_weak(
					s, 0, std::memory_order_relaxed
				);
				if (cas)
					return true;
			}
		}
	}

	// This function posts the log record to a per-CPU ring buffer.
	// If expedited is true, this function always emits logs within this context,
	// using LogHandler::emitUrgent() as necessary.
	void postLogRecord(frg::string_view record, bool expedited) {
		StatelessIrqLock irqLock;
		auto *cpuData = getCpuData();

		// If true, the usual logging path (i.e., emitLogsFromRing()) is bypassed;
		// instead, the record is directly sent to LoggingSink::emitUrgent().
		bool emitUrgent = false;

		// If checkEmitting() is true, emitLogsFromRing() would not be able to emit.
		// For example, this can happen when we use urgentLogger() or panicLogger() in
		// NMI contexts.
		if (expedited && checkEmitting())
			emitUrgent = true;

		if (!emitUrgent) {
			cpuData->localLogRing->enqueue(record.data(), record.size());

			// If the expedited flag is set, we always emit logs.
			// This is the path that kernel panics should usually take.
			bool avoidEmittingLogs = cpuData->avoidEmittingLogs.load(std::memory_order_relaxed);
			if (!avoidEmittingLogs || expedited)
				emitLogsFromRing();

			// TODO: If we do not call into emitLogsFromRing() here,
			//       we should wake up a (kernel) thread that emits the logs.
		} else {
			if (record.size() < sizeof(LogMetadata))
				panic();
			// TODO: Iterating through globalLogList without locks is unsafe.
			//       Fix this by using a lock-free data structure.
			for (const auto &it : *globalLogList) {
				if (!it->takesUrgentLogs)
					continue;
				it->emitUrgent(record);
			}
		}
	}

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
