#include <thor-internal/core.hpp>
#include <thor-internal/debug.hpp>

namespace thor {

constinit OutputSink infoSink;

constinit frg::stack_buffer_logger<LogSink> infoLogger;
constinit frg::stack_buffer_logger<PanicSink> panicLogger;

static constinit IrqSpinlock logMutex;

void LogSink::operator() (const char *msg) {
	auto lock = frigg::guard(&logMutex);
	infoSink.print(msg);
	infoSink.print('\n');
}

void PanicSink::operator() (const char *msg) {
	auto lock = frigg::guard(&logMutex);
	infoSink.print(msg);
	infoSink.print('\n');
}

struct LogMessage {
	char text[100];
};

size_t currentLogLength;
LogMessage logQueue[1024];
size_t logHead;

size_t currentLogSequence() {
	return logHead;
}

void copyLogMessage(size_t sequence, char *text) {
	memcpy(text, logQueue[sequence % 1024].text, 100);
}

frigg::LazyInitializer<frg::intrusive_list<
	LogHandler,
	frg::locate_member<
		LogHandler,
		frg::default_list_hook<LogHandler>,
		&LogHandler::hook
	>
>> globalLogList;

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
	constexpr int maximalCsiLength = 16;
	char csiBuffer[maximalCsiLength];
	int csiState;
	int csiLength;
} // namespace anonymous

void OutputSink::print(char c) {
	auto doesFit = [] (int n) -> bool {
		return currentLogLength + n < 100;
	};

	auto cutOff = [] () {
		currentLogLength = 0;
		logHead++;
		memset(logQueue[logHead % 1024].text, 0, 100);
		for (const auto &it : *globalLogList)
			it->printChar('\n');
	};

	auto emit = [] (char c) {
		logQueue[logHead % 1024].text[currentLogLength] = c;
		currentLogLength++;
		for (const auto &it : *globalLogList)
			it->printChar(c);
	};

	if(!csiState) {
		if(c == '\x1B') {
			csiState = 1;
		}else if(c == '\n' || !doesFit(1)) {
			cutOff();
		}else{
			emit(c);
		}
	}else if(csiState == 1) {
		if(c == '[') {
			csiState = 2;
		}else{
			if(!doesFit(2)) {
				cutOff();
			}else{
				emit('\x1B');
				emit(c);
			}
			csiState = 0;
		}
	}else{
		// This is csiState == 2.
		if((c >= '0' && c <= '9') || (c == ';')) {
			if(csiLength < maximalCsiLength)
				csiBuffer[csiLength] = c;
			csiLength++;
		}else{
			if(csiLength >= maximalCsiLength || !doesFit(3 + csiLength)) {
				cutOff();
			}else{
				emit('\x1B');
				emit('[');
				for(int i = 0; i < csiLength; i++)
					emit(csiBuffer[i]);
				emit(c);
			}
			csiState = 0;
			csiLength = 0;
		}
	}
}

void OutputSink::print(const char *str) {
	while(*str != 0)
		print(*str++);
}

} // namespace thor
