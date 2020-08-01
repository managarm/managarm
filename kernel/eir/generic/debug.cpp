#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/arch.hpp>
#include <frg/logging.hpp>
#include <render-text.hpp>

namespace eir {

constinit OutputSink infoSink;
constinit frg::stack_buffer_logger<LogSink> infoLogger;
constinit frg::stack_buffer_logger<PanicSink> panicLogger;

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

void OutputSink::print(char c) {
	debugPrintChar(c);

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
	infoSink.print('\n');
	while(true);
}

} // namespace eir

// Frigg glue functions

void friggBeginLog() { }
void friggEndLog() { }
void friggPrintCritical(char c) { eir::infoSink.print(c); }
void friggPrintCritical(char const *c) { eir::infoSink.print(c); }
void friggPanic() { while(true); }
