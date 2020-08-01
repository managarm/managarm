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
};

extern frg::stack_buffer_logger<LogSink> infoLogger;
extern frg::stack_buffer_logger<PanicSink> panicLogger;

} // namespace eir
