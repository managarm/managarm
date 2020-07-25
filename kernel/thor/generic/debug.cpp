#include <thor-internal/core.hpp>
#include <thor-internal/debug.hpp>

namespace thor {

constinit BochsSink infoSink;

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

} // namespace thor
