
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/initializer.hpp>
#include <frigg/support.hpp>
#include <frigg/debug.hpp>

namespace frigg {
namespace debug {

PanicLogger panicLogger;

// --------------------------------------------------------
// PanicLogger
// --------------------------------------------------------

PanicLogger::Printer PanicLogger::log() {
	friggPrintCritical("Panic!\n");
	return Printer();
}

// --------------------------------------------------------
// PanicLogger::Printer
// --------------------------------------------------------

void PanicLogger::Printer::print(char c) {
	friggPrintCritical(c);
}
void PanicLogger::Printer::print(const char *str) {
	friggPrintCritical(str);
}

void PanicLogger::Printer::finish() {
	friggPrintCritical('\n');
	friggPanic();
}

// --------------------------------------------------------
// Namespace scope functions
// --------------------------------------------------------

void assertionFail(const char *message, const char *function) {
	PanicLogger logger;
	logger.log() << "Assertion failed: " << message << "\n"
			<< "In function " << function << Finish();
}

}} // namespace frigg::debug

extern "C" void __cxa_pure_virtual() {
	frigg::debug::PanicLogger logger;
	logger.log() << "Pure virtual call" << frigg::debug::Finish();
}

