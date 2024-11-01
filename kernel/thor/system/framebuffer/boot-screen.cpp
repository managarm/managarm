
#include <thor-internal/framebuffer/boot-screen.hpp>

namespace thor {

BootScreen::Formatter::Formatter(BootScreen *screen, size_t x, size_t y)
    : _screen{screen},
      _csiState{0},
      _modeCount{0},
      _x{x},
      _y{y} {
	for (size_t i = 0; i < 4; i++)
		_modeStack[i] = 0;
}

void BootScreen::Formatter::print(const char *c) {
	while (*c) {
		if (!_csiState) {
			if (*c == '\x1B') {
				_csiState = 1;
				c++;
			} else if (*c == '\t') {
				constexpr const char *spaces = "        ";

				size_t n = 8 - (_x % 8);
				if (!n)
					n = 8;

				int m = frg::min(_screen->_width - _x, n);
				if (m) {
					_screen->_display->setChars(_x, _y, spaces, m, _fg, _bg);
					_x += m;
				}

				c++;
			} else {
				size_t n = 0;
				while (c[n] && c[n] != '\x1B')
					n++;
				int m = frg::min(_screen->_width - _x, n);
				if (m) {
					_screen->_display->setChars(_x, _y, c, m, _fg, _bg);
					_x += m;
				}
				c += n;
			}
		} else if (_csiState == 1) {
			if (*c == '[') {
				_csiState = 2;
				c++;
			} else {
				// TODO: ESC should never be emitted (?).
				_screen->_display->setChars(_x, _y, c, 1, _fg, _bg);
				_csiState = 0;
				c++;
			}
		} else {
			// This is _csiState == 2.
			if (*c >= '0' && *c <= '9') {
				_modeStack[_modeCount] *= 10;
				_modeStack[_modeCount] += *c - '0';
				c++;
			} else if (*c == ';') {
				_modeCount++;
				c++;
			} else {
				if (*c == 'm') {
					for (int i = 0; i <= _modeCount; i++) {
						if (!_modeStack[i]) {
							_fg = _initialFg;
						} else if (_modeStack[i] >= 30 && _modeStack[i] <= 37) {
							_fg = _modeStack[i] - 30;
						} else if (_modeStack[i] == 39) {
							_fg = _initialFg;
						}
					}
				}

				for (int i = 0; i < 4; i++)
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
    : _display{display},
      _bottomSequence{0},
      _fmt{this, 0, 0} {
	_width = _display->getWidth();
	_height = _display->getHeight();
}

void BootScreen::printChar(char) {
	auto displayLine = [&](uint64_t seq, size_t i) {
		LogMessage log;
		copyLogMessage(seq, log);
		_fmt = Formatter{this, 0, i};
		_fmt.print(log.text);
	};

	if (auto cs = currentLogSequence(); _bottomSequence < cs) {
		// Fully redraw the first _height - 1 lines.
		for (size_t i = 1; i < _height; i++) {
			if (cs < i)
				break;
			displayLine(cs - i, _height - 1 - i);
		}

		// Clear the last line.
		_bottomSequence = cs;
		_display->setBlanks(0, _height - 1, frg::min(logLineLength, _width), -1);
		_fmt = Formatter{this, 0, _height - 1};
	}

	// Partially draw the last line.
	//  LogMessage log;
	//	copyLogMessage(_bottomSequence, log);
	//	_fmt.print(text + _x);
}

void BootScreen::setPriority(Severity prio) {
	switch (prio) {
	case Severity::emergency:
	case Severity::alert:
	case Severity::critical:
	case Severity::error:
		_fmt.print("\e[31m");
		break;
	case Severity::warning:
		_fmt.print("\e[33m");
		break;
	case Severity::notice:
	case Severity::info:
		_fmt.print("\e[37m");
		break;
	case Severity::debug:
		_fmt.print("\e[35m");
		break;
	default:
		_fmt.print("\e[39m");
		break;
	}
}

void BootScreen::resetPriority() { _fmt.print("\e[39m"); }

void BootScreen::printString(const char *string) {
	while (*string)
		printChar(*string++);
}

} // namespace thor
