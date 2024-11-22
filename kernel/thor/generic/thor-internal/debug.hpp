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

// Metadata struct that preceeds each log record within kernel ring buffers.
struct LogMetadata {
	Severity severity;
};

inline frg::tuple<LogMetadata, frg::string_view> destructureLogRecord(frg::string_view record) {
	if (record.size() < sizeof(LogMetadata))
		panic();
	LogMetadata md;
	memcpy(&md, record.data(), sizeof(LogMetadata));
	frg::string_view msg{record.data() + sizeof(LogMetadata), record.size() - sizeof(LogMetadata)};
	return {md, msg};
}

// Synchronous logging sink.
//
// Thread safety
// ---
// Both emit() and emitUrgent() can be called from arbitrary contexts (including NMI).
// Hence, these functions must ensure that they do not to take locks and that they do not rely
// on kernel infrastructure that takes locks.
// Logging sinks that make use of extensive kernel infrastructure should copy the logs
// to a ring buffer first and use a kernel thread to process them.
//
// Log messages
// ---
// Note that log messages are _not_ null-terminated, the handler has to respect the length.
// Also note that the message does not end with a newline.
struct LogHandler {
	// Writes a log message to this handler.
	//
	// emit() is called with a global logging mutex held;
	// in particular, all calls to emit() are serialized.
	virtual void emit(frg::string_view record) = 0;

	// Like emit() but logs out-of-band messages.
	// This is usually called in emergencies when the usual logging infrastrcture is broken.
	// emitUrgent() is only called on handlers that have takesUrgentLogs set.
	//
	// emitUrgent() is called without any mutexes held.
	// Hence, calls to emitUrgent() are not serialized.
	// The default implementation calls emit().
	virtual void emitUrgent(frg::string_view record);

	frg::default_list_hook<LogHandler> hook;

	bool takesUrgentLogs{false};

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
