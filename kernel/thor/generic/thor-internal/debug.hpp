#pragma once

#include <eir/interface.hpp>
#include <frg/list.hpp>
#include <frg/logging.hpp>
#include <thor-internal/elf-notes.hpp>

namespace thor {

void panic();

// Boot-time debug options populated by Eir via the debugOptions ELF note.
extern ManagarmElfNote<DebugOptions> debugOptionsNote;

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
// Logging sinks that make use of extensive kernel infrastructure should perform their work
// on a kernel thread instead.
//
// Log messages
// ---
// Note that log messages are _not_ null-terminated, the handler has to respect the length.
// Also note that the message does not end with a newline.
struct LogHandler {
	// Writes a log message to this handler.
	//
	// This is called with the global logging mutex held.
	// Calls to emit() and flush() are serialized.
	virtual void emit(frg::string_view record) = 0;

	// Called after a batch of emit() calls to allow the handler to flush/redraw.
	//
	// This is called with the global logging mutex held.
	// Calls to emit() and flush() are serialized.
	virtual void flush() {}

	// Like emit() for logs urgent messages.
	// emitUrgent() is only called on handlers that have takesUrgentLogs set.
	// The default implementation calls emit().
	//
	// This is called with the global logging mutex held but not serialized w.r.t. reentrancy:
	// emitUrgent() may be called in an exception or NMI while an outer frame is currently
	// calling into another LogHandler function (i.e., emit(), flush(), emitUrgent()) on the same CPU.
	virtual void emitUrgent(frg::string_view record);

	frg::intrusive_rcu_list_hook<LogHandler> hook;

	bool takesUrgentLogs{false};

protected:
	~LogHandler() = default;
};

// Exposed for read access only.
// Modifications must use enableLogHandler() / disableLogHandler().
// TODO: If frigg exposed const iterators for intrusive_rcu_list,
//       we could replace this by a function that returns a const reference.
extern frg::intrusive_rcu_list<
	LogHandler,
	frg::locate_member<
		LogHandler,
		frg::intrusive_rcu_list_hook<LogHandler>,
		&LogHandler::hook
	>
> globalLogList;

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
