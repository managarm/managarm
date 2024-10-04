#pragma once

#include <frg/formatting.hpp>
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
extern void (*logHandler)(const char c);

extern frg::stack_buffer_logger<LogSink, 128> infoLogger;
extern frg::stack_buffer_logger<PanicSink, 128> panicLogger;

} // namespace eir
