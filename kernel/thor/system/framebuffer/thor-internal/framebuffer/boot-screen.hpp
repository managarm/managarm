#pragma once

#include <thor-internal/debug.hpp>

namespace thor {

struct TextDisplay {
	virtual size_t getWidth() = 0;
	virtual size_t getHeight() = 0;

	virtual void setChars(unsigned int x, unsigned int y,
			const char *c, int count, int fg, int bg) = 0;
	virtual void setBlanks(unsigned int x, unsigned int y, int count, int bg) = 0;

protected:
	~TextDisplay() = default;
};

struct BootScreen final : public LogHandler {
	struct Formatter {
		Formatter(BootScreen *screen, size_t x, size_t y);

		void print(const char *c);

	private:
		BootScreen *_screen;

		int _csiState;
		int _modeStack[4];
		int _modeCount;

		size_t _x;
		size_t _y;
		int _fg = 15;
		int _bg = -1;
		int _initialFg = _fg;
	};

	BootScreen(TextDisplay *display);

	void printChar(char c) override;
	void setPriority(Severity prio) override;
	void resetPriority() override;

	void printString(const char *string);

private:
	TextDisplay *_display;
	size_t _width;
	size_t _height;

	uint64_t _bottomSequence;
	Formatter _fmt;
};

} // namespace thor
