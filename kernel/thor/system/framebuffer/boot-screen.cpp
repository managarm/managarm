
#include <thor-internal/framebuffer/boot-screen.hpp>

namespace thor {

BootScreen::Formatter::Formatter(BootScreen *screen, int x, int y)
: _screen{screen}, _csiState{0}, _modeCount{0}, _x{x}, _y{y} {
	for(int i = 0; i < 4; i++)
		_modeStack[i] = 0;
}

void BootScreen::Formatter::print(const char *c) {
	while(*c) {
		if(!_csiState) {
			if(*c == '\x1B') {
				_csiState = 1;
				c++;
			}else{
				int n = 0;
				while(c[n] && c[n] != '\x1B')
					n++;
				int m = frg::min(_screen->_width - _x, n);
				if(m) {
					_screen->_display->setChars(_x, _y, c, m, _fg, _bg);
					_x += m;
				}
				c += n;
			}
		}else if(_csiState == 1) {
			if(*c == '[') {
				_csiState = 2;
				c++;
			}else{
				// TODO: ESC should never be emitted (?).
				_screen->_display->setChars(_x, _y, c, 1, _fg, _bg);
				_csiState = 0;
				c++;
			}
		}else{
			// This is _csiState == 2.
			if(*c >= '0' && *c <= '9') {
				_modeStack[_modeCount] *= 10;
				_modeStack[_modeCount] += *c - '0'; 
				c++;
			}else if(*c == ';') {
				_modeCount++;
				c++;
			}else{
				if(*c == 'm') {
					for(int i = 0; i <= _modeCount; i++) {
						if(!_modeStack[i]) {
							_fg = 15;
						}else if(_modeStack[i] >= 30 && _modeStack[i] <= 37) {
							_fg = _modeStack[i] - 30;
						}else if(_modeStack[i] == 39) {
							_fg = 15;
						}
					}
				}
		
				for(int i = 0; i < 4; i++)
					_modeStack[i] = 0;
				_modeCount = 0;
				
				_csiState = 0;
				c++;
			}
		}
	}

	_screen->_display->setBlanks(_x, _y, _screen->_width - _x, _bg);
}

BootScreen::BootScreen(TextDisplay *display)
: _display{display}, _bottomSequence{0}, _fmt{this, 0, 0} {
	_width = _display->getWidth();
	_height = _display->getHeight();
}

void BootScreen::printChar(char c) {
	auto displayLine = [&] (uint64_t seq, int i) {
		char text[100];
		copyLogMessage(seq, text);
		_fmt = Formatter{this, 0, i};
		_fmt.print(text);
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
		_display->setBlanks(0, _height - 1, frg::min(100, _width), -1);
		_fmt = Formatter{this, 0, _height - 1};
	}

	// Partially draw the last line.
//	char text[100];
//	copyLogMessage(_bottomSequence, text);
//	_fmt.print(text + _x);
}

void BootScreen::printString(const char *string) {
	while(*string)
		printChar(*string++);
}

} //namespace thor

