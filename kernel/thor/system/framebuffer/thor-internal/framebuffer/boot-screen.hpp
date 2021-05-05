#pragma once

#include <thor-internal/debug.hpp>

namespace thor {

struct TextDisplay {
	virtual int getWidth() = 0;
	virtual int getHeight() = 0;

	virtual void setChars(unsigned int x, unsigned int y,
			const char *c, int count, int fg, int bg) = 0;
	virtual void setBlanks(unsigned int x, unsigned int y, int count, int bg) = 0;

protected:
	~TextDisplay() = default;
};

struct BootScreen final : LogHandler {
	struct Formatter {
		Formatter(BootScreen *screen, int x, int y);

		void print(const char *c);

	private:
		BootScreen *_screen;
		
		int _csiState;
		int _modeStack[4];
		int _modeCount;
		
		int _x;
		int _y;
		int _fg = 15;
		int _bg = -1;
	};

	BootScreen(TextDisplay *display);

	void printChar(char c) override;
	void printString(const char *string);

private:
	TextDisplay *_display;
	int _width;
	int _height;

	uint64_t _bottomSequence;
	Formatter _fmt;
};

} // namespace thor
