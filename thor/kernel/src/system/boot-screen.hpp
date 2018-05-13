
#ifndef THOR_SRC_SYSTEM_BOOT_SCREEN
#define THOR_SRC_SYSTEM_BOOT_SCREEN

#include "../generic/core.hpp"

namespace thor {

struct TextDisplay {
	virtual void setChar(unsigned int x, unsigned int y, char c, int fg, int bg) = 0;
	virtual int getWidth() = 0;
	virtual int getHeight() = 0;
};

struct BootScreen : LogHandler {
	BootScreen(TextDisplay *display)
	:_display(display) { }

	void printChar(char c) override;
	void printString(const char *string);

private:
	TextDisplay *_display;
	int _x = 0;
	int _y = 0;
	int _fg = 15;
	int _bg = -1;
};

} // namespace thor

#endif // THOR_SRC_SYSTEM_BOOT_SCREEN

