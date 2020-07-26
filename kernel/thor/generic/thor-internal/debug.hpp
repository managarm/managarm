#pragma once

#include <frg/list.hpp>
#include <frg/logging.hpp>

namespace thor {

void panic();

// --------------------------------------------------------
// Log infrastructure.
// --------------------------------------------------------

class OutputSink {
public:
	void print(char c);
	void print(const char *str);
};

extern OutputSink infoSink;

struct LogHandler {
	virtual void printChar(char c) = 0;

	frg::default_list_hook<LogHandler> hook;
};

void enableLogHandler(LogHandler *sink);
void disableLogHandler(LogHandler *sink);

size_t currentLogSequence();
void copyLogMessage(size_t sequence, char *text);

// --------------------------------------------------------
// Loggers.
// --------------------------------------------------------

struct LogSink {
	constexpr LogSink() = default;

	void operator() (const char *msg);
};

struct PanicSink {
	constexpr PanicSink() = default;

	void operator() (const char *msg);
};

extern frg::stack_buffer_logger<LogSink> infoLogger;
extern frg::stack_buffer_logger<PanicSink> panicLogger;

} // namespace thor
