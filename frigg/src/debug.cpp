
#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>
#include <frigg/support.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>

#ifdef FRIGG_NO_LIBC

extern "C" void __assert_fail(const char *assertion, const char *file,
		unsigned int line, const char *function) {
	frigg::PanicLogger logger;
	logger() << "Assertion failed: " << assertion << "\n"
			<< "In function " << function
			<< " at " << file << ":" << line << frigg::endLog;
}

extern "C" void __cxa_pure_virtual() {
	frigg::PanicLogger logger;
	logger() << "Pure virtual call" << frigg::endLog;
}

#endif // FRIGG_NO_LIBC

