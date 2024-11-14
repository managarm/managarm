#pragma once

#include <frg/list.hpp>
#include <frg/logging.hpp>

namespace thor {

void panic();

// --------------------------------------------------------
// Log infrastructure.
// --------------------------------------------------------

constexpr size_t logLineLength = 256;

// see RFC 5424
enum class Severity : uint8_t {
	emergency,
	alert,
	critical,
	error,
	warning,
	notice,
	info,
	debug,
};

struct LogHandler {
	// Writes a log message to this handler.
	// Note that the message is _not_ null-terminated, the handler has to respect the length.
	// Also note that the message does not end with a newline.
	virtual void emit(Severity severity, frg::string_view msg) = 0;

	frg::default_list_hook<LogHandler> hook;

protected:
	~LogHandler() = default;
};

void enableLogHandler(LogHandler *sink);
void disableLogHandler(LogHandler *sink);

// --------------------------------------------------------
// Loggers.
// --------------------------------------------------------

struct DebugSink {
	constexpr DebugSink() = default;

	void operator() (const char *msg);
};

struct WarningSink {
	constexpr WarningSink() = default;

	void operator() (const char *msg);
};

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

extern frg::stack_buffer_logger<DebugSink, logLineLength> debugLogger;
extern frg::stack_buffer_logger<WarningSink, logLineLength> warningLogger;
extern frg::stack_buffer_logger<InfoSink, logLineLength> infoLogger;
// Similar in spirit as infoLogger(), but avoids the use of sophisticated kernel infrastructure.
// This can be used to debug low-level kernel infrastructure, e.g., irqMutex().
extern frg::stack_buffer_logger<UrgentSink, logLineLength> urgentLogger;
extern frg::stack_buffer_logger<PanicSink, logLineLength> panicLogger;

} // namespace thor
