#pragma once

#include <frg/formatting.hpp>
#include <frg/list.hpp>
#include <frg/logging.hpp>

namespace eir {

struct OutputSink {
	void print(char c);
	void print(const char *c);
};

struct LogSink {
	void operator()(const char *c);
};

struct PanicSink {
	void operator()(const char *c);
	void finalize(bool);
};

extern bool log_e9;

extern frg::stack_buffer_logger<LogSink, 128> infoLogger;
extern frg::stack_buffer_logger<PanicSink, 128> panicLogger;

struct LogHandler {
	virtual void emit(frg::string_view line) = 0;

	frg::default_list_hook<LogHandler> hook;
	bool active{false};
};

void enableLogHandler(LogHandler *handler);
void disableLogHandler(LogHandler *handler);

} // namespace eir
