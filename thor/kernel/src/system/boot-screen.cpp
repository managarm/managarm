
#include <frigg/debug.hpp>
#include "boot-screen.hpp"

namespace thor {

void BootScreen::printChar(char c) {
	if(_y >= _display->getHeight()) {
		for(size_t y = 0; y < _display->getHeight() - 1; y++) {
			char text[100];
			copyLogMessage(currentLogSequence() - _display->getHeight() + y, text);
			for(size_t x = 0; x < 100 && x < _display->getWidth(); x++) {
				if(!text[x]) {
					_display->setChar(x, y, ' ', _fg, _bg);
				}else {
					_display->setChar(x, y, text[x], _fg, _bg);
				}

			}
		}
		for(size_t x = 0; x < _display->getWidth(); x++) {
			_display->setChar(x, _display->getHeight() - 1, ' ', _fg, _bg);
		}

		_y = _display->getHeight() - 1;
	}

	if(c == '\n') {
		_y++;
		_x = 0;
		return;
	}

	_display->setChar(_x, _y, c, _fg, _bg);
	_x++;
	if(_x >= _display->getWidth()) {
		_x = 0;
		_y++;
	}
}

void BootScreen::printString(const char *string) {
	while(*string)
		printChar(*string++);
}

} //namespace thor

