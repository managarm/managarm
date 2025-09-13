#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <frg/logging.hpp>

namespace eir {

namespace {

using HandlerList = frg::intrusive_list<
    LogHandler,
    frg::locate_member<LogHandler, frg::default_list_hook<LogHandler>, &LogHandler::hook>>;

HandlerList &accessHandlerList() {
	static frg::eternal<HandlerList> singleton;
	return *singleton;
}

} // anonymous namespace

bool log_e9 = false;

constinit OutputSink infoSink;
constinit frg::stack_buffer_logger<LogSink, 128> infoLogger;
constinit frg::stack_buffer_logger<PanicSink, 128> panicLogger;

extern "C" void frg_panic(const char *cstring) {
	panicLogger() << "frg: Panic! " << cstring << frg::endlog;
}

void OutputSink::print(char c) {
	// Emit to the platform-specific device.
	// For example, this can log to SBI on RISC-V which often yields expected results.
	// Also, it can log to virtual devices (like e9) when run inside VMs.
	debugPrintChar(c);
}

void OutputSink::print(const char *str) {
	auto &handlerList = accessHandlerList();
	for (auto *handler : handlerList) {
		handler->emit(str);
	}

	while (*str)
		print(*(str++));
}

void LogSink::operator()(const char *c) {
	infoSink.print(c);
	infoSink.print('\n');
}

void PanicSink::operator()(const char *c) { infoSink.print(c); }

void PanicSink::finalize(bool) {
	infoSink.print('\n');
	while (true)
		asm volatile("" : : : "memory");
}

void enableLogHandler(LogHandler *handler) {
	if (handler->active)
		return;

	auto &handlerList = accessHandlerList();
	handlerList.push_back(handler);
	handler->active = true;
}

void disableLogHandler(LogHandler *handler) {
	if (!handler->active)
		return;

	auto &handlerList = accessHandlerList();
	auto it = handlerList.iterator_to(handler);
	handlerList.erase(it);
	handler->active = false;
}

} // namespace eir

extern "C" void
__assert_fail(const char *assertion, const char *file, unsigned int line, const char *function) {
	eir::panicLogger() << "Assertion failed: " << assertion << "\n"
	                   << "In function " << function << " at " << file << ":" << line
	                   << frg::endlog;
}

extern "C" void __cxa_pure_virtual() { eir::panicLogger() << "Pure virtual call" << frg::endlog; }
