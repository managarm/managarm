#pragma once

#include <frg/list.hpp>
#include <frg/logging.hpp>

namespace thor {

void panic();

// --------------------------------------------------------
// Log infrastructure.
// --------------------------------------------------------

constexpr int logLineLength = 256;

struct LogMessage {
	char text[logLineLength];
};

struct LogHandler {
	virtual void printChar(char c) = 0;

	frg::default_list_hook<LogHandler> hook;

protected:
	~LogHandler() = default;
};

void enableLogHandler(LogHandler *sink);
void disableLogHandler(LogHandler *sink);

size_t currentLogSequence();
void copyLogMessage(size_t sequence, LogMessage &msg);

// --------------------------------------------------------
// Loggers.
// --------------------------------------------------------

struct InfoSink {
	constexpr InfoSink() = default;

	void operator() (const char *msg);
};

struct UrgentSink {
	constexpr UrgentSink() = default;

	void operator() (const char *msg);
};

struct PanicSink {
	constexpr PanicSink() = default;

	void operator() (const char *msg);
	void finalize(bool);
};

extern frg::stack_buffer_logger<InfoSink, logLineLength> infoLogger;
// Similar in spirit as infoLogger(), but avoids the use of sophisticated kernel infrastructure.
// This can be used to debug low-level kernel infrastructure, e.g., irqMutex().
extern frg::stack_buffer_logger<UrgentSink, logLineLength> urgentLogger;
extern frg::stack_buffer_logger<PanicSink, logLineLength> panicLogger;

} // namespace thor
