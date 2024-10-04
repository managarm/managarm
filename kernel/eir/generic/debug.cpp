#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/arch.hpp>
#include <frg/logging.hpp>
#include <render-text.hpp>

namespace eir {

bool log_e9 = false;
void (*logHandler)(const char c) = nullptr;

constinit OutputSink infoSink;
constinit frg::stack_buffer_logger<LogSink, 128> infoLogger;
constinit frg::stack_buffer_logger<PanicSink, 128> panicLogger;

constexpr int fontWidth = 8;
constexpr int fontHeight = 16;

static void *displayFb;
static int displayWidth;
static int displayHeight;
static size_t displayPitch;
static int outputX;
static int outputY;

void setFbInfo(void *ptr, int width, int height, size_t pitch) {
	displayFb = ptr;
	displayWidth = width;
	displayHeight = height;
	displayPitch = pitch;
}

extern "C" void frg_panic(const char *cstring) {
	panicLogger() << "frg: Panic! " << cstring << frg::endlog;
}

void OutputSink::print(char c) {
	if(log_e9)
		debugPrintChar(c);

	if(logHandler)
		logHandler(c);

	if(displayFb) {
		if(c == '\n') {
			outputX = 0;
			outputY++;
		}else if(outputX >= displayWidth / fontWidth) {
			outputX = 0;
			outputY++;
		}else if(outputY >= displayHeight / fontHeight) {
			// TODO: Scroll.
		}else{
			renderChars(displayFb, displayPitch / sizeof(uint32_t),
					outputX, outputY, &c, 1, 15, -1,
					std::integral_constant<int, fontWidth>{},
					std::integral_constant<int, fontHeight>{});
			outputX++;
		}
	}
}

void OutputSink::print(const char *str) {
	while(*str)
		print(*(str++));
}


void LogSink::operator()(const char *c) {
	infoSink.print(c);
	infoSink.print('\n');
}

void PanicSink::operator()(const char *c) {
	infoSink.print(c);
}

void PanicSink::finalize(bool) {
	infoSink.print('\n');
	while(true)
		asm volatile("" : : : "memory");
}

} // namespace eir

extern "C" void __assert_fail(const char *assertion, const char *file,
		unsigned int line, const char *function) {
	eir::panicLogger() << "Assertion failed: " << assertion << "\n"
			<< "In function " << function
			<< " at " << file << ":" << line << frg::endlog;
}

extern "C" void __cxa_pure_virtual() {
	eir::panicLogger() << "Pure virtual call" << frg::endlog;
}
