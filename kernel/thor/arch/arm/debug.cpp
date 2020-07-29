#include <thor-internal/arch/debug.hpp>

namespace thor {

constinit DummyLogHandler dummyLogHandler;

void setupDebugging() {
	enableLogHandler(&dummyLogHandler);
}

void DummyLogHandler::printChar(char c) {
}

} // namespace thor
