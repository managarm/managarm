
#include "../../frigg/include/arch_x86/types64.hpp"
#include "util/general.hpp"
#include "runtime.hpp"
#include "debug.hpp"

namespace thor {
namespace debug {

Logger *criticalLogger;

void panic() {
	criticalLogger->log("Kernel panic!");
	thorRtHalt();
}

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
		: p_screen(screen) { }

void Terminal::advanceCursor() {
	p_screen->setCursor(p_screen->getCursorX() + 1, p_screen->getCursorY());
}

void Terminal::printChar(char c) {
	if(c == '\n') {
	p_screen->setCursor(0, p_screen->getCursorY() + 1);
	}else{
		p_screen->setChar(c);
		advanceCursor();
	}
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
// Logger
// --------------------------------------------------------

void Logger::log(const char *string) {
	while(*string != 0) {
		print(*string);
		string++;
	}
	print('\n');
}

void Logger::log(const char *string, size_t length) {
	for(size_t i = 0; i < length; i++) {
		print(string[i]);
	}
	print('\n');
}

void Logger::log(void *pointer) {
	print('0');
	print('x');
	logUInt((uintptr_t)pointer, 16);
	print('\n');
}

void Logger::log(int number) {
	logUInt(number, 10);
	print('\n');
}

void Logger::logHex(int number) {
	print('0');
	print('x');
	logUInt(number, 16);
	print('\n');
}

// --------------------------------------------------------
// TerminalLogger
// --------------------------------------------------------

TerminalLogger::TerminalLogger(Terminal *terminal)
		: p_terminal(terminal) { }

void TerminalLogger::print(char c) {
	p_terminal->printChar(c);
}

}} // namespace thor::debug

