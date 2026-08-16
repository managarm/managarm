
#include <thor-internal/framebuffer/boot-screen.hpp>
#include <thor-internal/kernel-log.hpp>

namespace thor {

BootScreen::Formatter::Formatter(BootScreen *screen, size_t x, size_t y)
: _screen{screen}, _csiState{0}, _modeCount{0}, _x{x}, _y{y} {
	for(size_t i = 0; i < 4; i++)
		_modeStack[i] = 0;
}

void BootScreen::Formatter::print(const char *c) {
	print(c, strlen(c));
}

void BootScreen::Formatter::print(const char *c, size_t n) {
	const char *l = c + n;
	while(c != l) {
		if(!_csiState) {
			if(*c == '\x1B') {
				_csiState = 1;
				c++;
			}else if(*c == '\t') {
				constexpr const char *spaces = "        ";

				size_t n = 8 - (_x % 8);
				if (!n)
					n = 8;

				int m = frg::min(_screen->_width - _x, n);
				if(m) {
					_screen->_display->setChars(_x, _y, spaces, m, _fg, _bg);
					_x += m;
				}

				c++;
			}else{
				size_t n = 0;
				while(c + n != l && c[n] != '\x1B')
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
							_fg = _initialFg;
						}else if(_modeStack[i] >= 30 && _modeStack[i] <= 37) {
							_fg = _modeStack[i] - 30;
						}else if(_modeStack[i] == 39) {
							_fg = _initialFg;
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
: _display{display} {
	_width = _display->getWidth();
	_height = _display->getHeight();
}

void BootScreen::emit(frg::string_view) {
	// postLogRecord() posted the record to the log ring already; redraw() renders from there.
}

void BootScreen::flush() {
	redraw();
}

void BootScreen::redraw() {
	char buffer[logLineLength];

	// The last row is always left blank.
	auto numRows = _height - 1;

	// Drop records until the tail of the log ring fits onto the screen.
	auto numRecords = countRecords();
	while(numRecords > numRows) {
		auto [success, recordPtr, nextPtr, size] = retrieveLogRecord(_topPtr, buffer, 0);
		if(!success)
			break;
		_topPtr = nextPtr;
		numRecords--;
	}

	// Render the records. Producers may append to the ring while we do so; those records
	// are picked up by the next redraw().
	size_t y = 0;
	auto ptr = _topPtr;
	while(y < numRows) {
		auto [success, recordPtr, nextPtr, size] = retrieveLogRecord(ptr, buffer, logLineLength);
		if(!success)
			break;

		renderLine(y, {buffer, size});
		ptr = nextPtr;
		y++;
	}

	// Clear the rows that no record is rendered to.
	for(; y < _height; y++)
		_display->setBlanks(0, y, _width, -1);
}

size_t BootScreen::countRecords() {
	// Only the record boundaries are of interest here; a maximal size of zero avoids copying.
	char dummy;

	size_t n = 0;
	auto ptr = _topPtr;
	while(true) {
		auto [success, recordPtr, nextPtr, size] = retrieveLogRecord(ptr, &dummy, 0);
		if(!success)
			break;

		// The ring may have invalidated the records that _topPtr points to.
		if(!n)
			_topPtr = recordPtr;
		ptr = nextPtr;
		n++;
	}
	return n;
}

void BootScreen::renderLine(size_t y, frg::string_view record) {
	auto [md, msg] = destructureLogRecord(record);

	Formatter fmt{this, 0, y};

	switch(md.severity) {
		case Severity::emergency:
		case Severity::alert:
		case Severity::critical:
		case Severity::error:
			fmt.print("\e[31m");
			break;
		case Severity::warning:
			fmt.print("\e[33m");
			break;
		case Severity::notice:
		case Severity::info:
			fmt.print("\e[37m");
			break;
		case Severity::debug:
			fmt.print("\e[35m");
			break;
		default:
			fmt.print("\e[39m");
	}

	fmt.print(msg.data(), msg.size());
}

} //namespace thor
