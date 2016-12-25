
#ifndef LIBTERMINAL_HPP
#define LIBTERMINAL_HPP

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <experimental/optional>
#include <string>
#include <vector>

namespace libterminal {

enum Color {
	kColorBlack,
	kColorRed,
	kColorGreen,
	kColorYellow,
	kColorBlue,
	kColorMagenta,
	kColorCyan,
	kColorWhite
};

struct Attribute {
	Color fgColor = kColorWhite;
	Color bgColor = kColorBlack;
};

struct Display {
	virtual void setChar(int x, int y, char c, Attribute attribute) = 0;
	virtual void setCursor(int x, int y) = 0;

	int width = 50;
	int height = 10;
};

struct Emulator {
	Emulator(Display *display);

	enum Status {
		kStatusNormal,
		kStatusEscape,
		kStatusCsi
	};

	void setChar(int x, int y, char c, Attribute attribute);
	void handleControlSeq(char character);
	void handleCsi(char character);
	void printChar(char character);
	void printString(std::string string);

	Display *display;
	Status status = kStatusNormal;
	std::vector<int> params;
	int cursorX = 0;
	int cursorY = 0;
	int width;
	int height;
	Attribute attribute;
	std::experimental::optional<int> currentNumber;
	Attribute *attributes;
	char *chars;
};

} // namespace libterminal

#endif // LIBTERMINAL_HPP
