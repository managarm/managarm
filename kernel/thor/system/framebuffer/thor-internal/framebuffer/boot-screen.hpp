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
		void print(const char *c, size_t n);

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

	void emit(frg::string_view record) override;

	void redraw();

private:
	struct Line {
		Severity severity;
		size_t length{0};
		char msg[logLineLength]{};
	};

	// Number of lines that are kept in memory. Must be power of 2.
	static constexpr size_t NUM_LINES = 128;

	TextDisplay *_display;
	size_t _width;
	size_t _height;
	Line _displayLines[NUM_LINES];
	uint64_t _displaySeq{0};
};

} // namespace thor
