#pragma once

#include <frg/list.hpp>
#include <frg/logging.hpp>

namespace thor {

void panic();

// --------------------------------------------------------
// Log infrastructure.
// --------------------------------------------------------

struct LogHandler {
	virtual void printChar(char c) = 0;

	frg::default_list_hook<LogHandler> hook;

protected:
	~LogHandler() = default;
};

void enableLogHandler(LogHandler *sink);
void disableLogHandler(LogHandler *sink);

size_t currentLogSequence();
void copyLogMessage(size_t sequence, char *text);

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
};

extern frg::stack_buffer_logger<InfoSink> infoLogger;
// Similar in spirit as infoLogger(), but avoids the use of sophisticated kernel infrastructure.
// This can be used to debug low-level kernel infrastructure, e.g., irqMutex().
extern frg::stack_buffer_logger<UrgentSink> urgentLogger;
extern frg::stack_buffer_logger<PanicSink> panicLogger;

} // namespace thor
