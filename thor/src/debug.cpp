
#include "../../frigg/include/types.hpp"
#include "util/general.hpp"
#include "runtime.hpp"
#include "debug.hpp"

namespace thor {
namespace debug {

LogSink *infoSink;
LazyInitializer<DefaultLogger> infoLogger;
LazyInitializer<PanicLogger> panicLogger;

// --------------------------------------------------------
// VgaScreen
// --------------------------------------------------------

VgaScreen::VgaScreen(char *pointer, int width, int height)
		: p_pointer(pointer), p_width(width), p_height(height),
		p_cursorX(0), p_cursorY(0) { }

int VgaScreen::getWidth() {
	return p_width;
}
int VgaScreen::getHeight() {
	return p_height;
}

int VgaScreen::getCursorX() {
	return p_cursorX;
}
int VgaScreen::getCursorY() {
	return p_cursorY;
}

void VgaScreen::setCursor(int x, int y) {
	p_cursorX = x;
	p_cursorY = y;
}
void VgaScreen::setChar(char c) {
	char *ptr = p_pointer + ((p_width * p_cursorY + p_cursorX) * 2);
	ptr[0] = c;
	ptr[1] = 0x0F;
}

// --------------------------------------------------------
// Terminal
// --------------------------------------------------------

Terminal::Terminal(Screen *screen)
: p_screen(screen) {
	clear();
}

void Terminal::print(char c) {
	if(c == '\n') {
		p_screen->setCursor(0, p_screen->getCursorY() + 1);
	}else{
		p_screen->setChar(c);
		advanceCursor();
	}
}

void Terminal::print(const char *str) {
	while(*str != 0)
		Terminal::print(*str++);
}

void Terminal::advanceCursor() {
	p_screen->setCursor(p_screen->getCursorX() + 1, p_screen->getCursorY());
}

void Terminal::clear() {
	for(int x = 0; x < p_screen->getWidth(); x++)
		for(int y = 0; y < p_screen->getHeight(); y++) {
			p_screen->setCursor(x, y);
			p_screen->setChar(' ');
		}
	
	p_screen->setCursor(0, 0);
}

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
	thorRtHalt();
}

// --------------------------------------------------------
// Namespace scope functions
// --------------------------------------------------------

void assertionFail(const char *message) {
	panicLogger->log() << "Assertion failed: " << message << Finish();
}

}} // namespace thor::debug

