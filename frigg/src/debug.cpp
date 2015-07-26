
#include "../include/types.hpp"
#include "../include/utils.hpp"
#include "../include/initializer.hpp"
#include "../include/debug.hpp"

namespace frigg {
namespace debug {

util::LazyInitializer<PanicLogger> panicLogger;

// --------------------------------------------------------
// DefaultLogger
// --------------------------------------------------------

DefaultLogger::DefaultLogger(LogSink *sink)
: p_sink(sink) { }

DefaultLogger::Printer DefaultLogger::log() {
	return Printer(p_sink);
}

// --------------------------------------------------------
// DefaultLogger::Printer
// --------------------------------------------------------

DefaultLogger::Printer::Printer(LogSink *sink)
: p_sink(sink) { }

void DefaultLogger::Printer::print(char c) {
	p_sink->print(c);
}
void DefaultLogger::Printer::print(const char *str) {
	p_sink->print(str);
}

void DefaultLogger::Printer::finish() {
	p_sink->print('\n');
}

// --------------------------------------------------------
// PanicLogger
// --------------------------------------------------------

PanicLogger::PanicLogger(LogSink *sink)
: p_sink(sink) { }

PanicLogger::Printer PanicLogger::log() {
	p_sink->print("Kernel panic!\n");
	return Printer(p_sink);
}

// --------------------------------------------------------
// PanicLogger::Printer
// --------------------------------------------------------

PanicLogger::Printer::Printer(LogSink *sink)
: p_sink(sink) { }

void PanicLogger::Printer::print(char c) {
	p_sink->print(c);
}
void PanicLogger::Printer::print(const char *str) {
	p_sink->print(str);
}

void PanicLogger::Printer::finish() {
	p_sink->print('\n');
	// TODO: invoke some user-supplied panic function
	while(true) { }
}

// --------------------------------------------------------
// Namespace scope functions
// --------------------------------------------------------

void assertionFail(const char *message) {
	panicLogger->log() << "Assertion failed: " << message << Finish();
}

}} // namespace frigg::debug

extern "C" void __cxa_pure_virtual() {
	frigg::debug::panicLogger->log() << "Pure virtual call"
			<< frigg::debug::Finish();
}

