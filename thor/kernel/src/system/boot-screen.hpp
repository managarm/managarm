
#ifndef THOR_SRC_SYSTEM_BOOT_SCREEN
#define THOR_SRC_SYSTEM_BOOT_SCREEN

#include "../generic/core.hpp"

namespace thor {

struct TextDisplay {
	virtual int getWidth() = 0;
	virtual int getHeight() = 0;

	virtual void setChars(unsigned int x, unsigned int y,
			char *c, int count, int fg, int bg) = 0;
	virtual void setBlanks(unsigned int x, unsigned int y, int count, int bg) = 0;
};

struct BootScreen : LogHandler {
	BootScreen(TextDisplay *display);

	void printChar(char c) override;
	void printString(const char *string);

private:
	TextDisplay *_display;
	int _width;
	int _height;

	uint64_t _bottomSequence;
	int _drawLength;

	int _fg = 15;
	int _bg = -1;
};

} // namespace thor

#endif // THOR_SRC_SYSTEM_BOOT_SCREEN

