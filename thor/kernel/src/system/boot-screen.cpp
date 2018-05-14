
#include <frigg/debug.hpp>
#include "boot-screen.hpp"

namespace thor {

BootScreen::BootScreen(TextDisplay *display)
: _display{display}, _bottomSequence{0} {
	_width = _display->getWidth();
	_height = _display->getHeight();
}

void BootScreen::printChar(char c) {
	auto displayLine = [&] (uint64_t seq, int i) {
		char text[100];
		copyLogMessage(seq, text);
		
		int k = 0;
		while(k < frigg::min(100, _width) && text[k])
			k++;
		_display->setChars(0, i, text, k, _fg, _bg);
		_display->setBlanks(k, i, frigg::min(100, _width) - k, _bg);
	};

	if(auto cs = currentLogSequence(); _bottomSequence < cs) {
		// Fully redraw the first _height - 1 lines.
		for(int i = 1; i < _height; i++) {
			if(cs < static_cast<uint64_t>(i))
				break;
			displayLine(cs - i, _height - 1 - i);
		}

		// Clear the last line.
		_bottomSequence = cs;
		_display->setBlanks(0, _height - 1, frigg::min(100, _width), _bg);
		_drawLength = 0;
	}

	// Partially draw the last line.
	char text[100];
	copyLogMessage(_bottomSequence, text);
	
	int k = 0;
	while(_drawLength + k < frigg::min(100, _width) && text[_drawLength + k])
		k++;
	_display->setChars(_drawLength, _height - 1, text + _drawLength, k, _fg, _bg);
	_drawLength += k;
}

void BootScreen::printString(const char *string) {
	while(*string)
		printChar(*string++);
}

} //namespace thor

