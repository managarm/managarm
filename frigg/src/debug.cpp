
#include "../include/types.hpp"
#include "../include/utils.hpp"
#include "../include/initializer.hpp"
#include "../include/support.hpp"
#include "../include/debug.hpp"

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

void assertionFail(const char *message) {
	PanicLogger logger;
	logger.log() << "Assertion failed: " << message << Finish();
}

}} // namespace frigg::debug

extern "C" void __cxa_pure_virtual() {
	frigg::debug::PanicLogger logger;
	logger.log() << "Pure virtual call" << frigg::debug::Finish();
}

