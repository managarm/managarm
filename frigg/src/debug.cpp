
#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>
#include <frigg/support.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>

namespace frigg {

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

void assertionFail(const char *message, const char *function,
		const char *file, int line) {
	PanicLogger logger;
	logger.log() << "Assertion failed: " << message << "\n"
			<< "In function " << function
			<< " at " << file << ":" << line << EndLog();
}

} // namespace frigg

#ifdef FRIGG_NO_LIBC

extern "C" void __assert_fail(const char *assertion, const char *file,
		unsigned int line, const char *function) {
	frigg::PanicLogger logger;
	logger.log() << "Assertion failed: " << assertion << "\n"
			<< "In function " << function
			<< " at " << file << ":" << line << frigg::EndLog();
}

extern "C" void __cxa_pure_virtual() {
	frigg::PanicLogger logger;
	logger.log() << "Pure virtual call" << frigg::EndLog();
}

#endif // FRIGG_NO_LIBC

