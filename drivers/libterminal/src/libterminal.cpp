
#include <libterminal.hpp>

namespace libterminal {

bool logSequences = false;

Emulator::Emulator(Display *display) {
	this->display = display;
	this->height = display->height;
	this->width = display->width;

	chars = new char[width * height];
	attributes = new Attribute[width * height];
}

void Emulator::setChar(int x, int y, char c, Attribute attribute) {
	attributes[y * x + x] = attribute;
	chars[y * x + x] = c;
	display->setChar(x, y, c, attribute);
}

void Emulator::handleControlSeq(char character) {
	switch(character)
	case 'A': {
		int n = 1;
		if(!params.empty())
			n = params[0];
		if(n == 0)
			n = 1;

		if(cursorY - n >= 0) {
			cursorY -= n;
		}else{
			cursorY = 0;
		}
		display->setCursor(cursorX, cursorY);
		break;
	}
	case 'B': {
		int n = 1;
		if(!params.empty())
			n = params[0];
		if(n == 0)
			n = 1;

		if(cursorY + n <= height) {
			cursorY += n;
		}else{
			cursorY = height;
		}
		display->setCursor(cursorX, cursorY);
		break;
	}
	case 'C': {
		int n = 1;
		if(!params.empty())
			n = params[0];
		if(n == 0)
			n = 1;

		if(cursorX + n <= width) {
			cursorX += n;
		}else{
			cursorX = width;
		}
		display->setCursor(cursorX, cursorY);
		break;
	}
	case 'D': {
		int n = 1;
		if(!params.empty())
			n = params[0];
		if(n == 0)
			n = 1;

		if(cursorX - n >= 0) {
			cursorX -= n;
		}else{
			cursorX = 0;
		}
		display->setCursor(cursorX, cursorY);
		break;
	}
	case 'E': {
		int n = 1;
		if(!params.empty())
			n = params[0];

		if(cursorY + n <= height) {
			cursorY += n;
		}else{
			cursorY = height;
		}
		cursorX = 0;
		display->setCursor(cursorX, cursorY);
		break;
	}
	case 'F': {
		int n = 1;
		if(!params.empty())
			n = params[0];

		if(cursorY - n >= 0) {
			cursorY -= n;
		}else{
			cursorY = 0;
		}
		cursorX = 0;
		display->setCursor(cursorX, cursorY);
	}
	case 'G': {
		int n = 0;
		if(!params.empty())
			n = params[0];

		if(n >= 0 && n <= width){
			cursorX = n;
		}
		display->setCursor(cursorX, cursorY);
		break;
	}
	case 'J': {
		int n = 0;
		if(!params.empty())
			n = params[0];

		Attribute attribute;
		switch(n) {
		case 0:
			for(int i = cursorX; i <= width; i++) {
				setChar(i, cursorY, ' ', attribute);
			}
			for(int i = cursorY + 1; i <= height; i++) {
				for(int j = 0; j < width; j++) {
					setChar(i, cursorY, ' ', attribute);
				}
			}
			break;
		}
		case 1:
			for(int i = cursorX; i >= 0; i--) {
					setChar(i, cursorY, ' ', attribute);
			}
			for(int i = cursorY - 1; i >= 0; i--) {
				for(int j = 0; j < width; j++) {
					setChar(i, cursorY, ' ', attribute);
				}
			}
			break;
		}
		case 2:
			for(int i = 0; i <= height; i++) {
				for(int j = 0; j <= width; j++) {
					setChar(i, cursorY, ' ', attribute);
				}
			}
			break;
		default:;
		}
		break;
	}
	case 'K': {
		int n = 0;
		if(!params.empty())
			n = params[0];

		Attribute attribute;
		switch(n) {
		case 0:	for(int i = cursorX; i < width; i++) {
				setChar(i, cursorY, ' ', attribute);
			}
			break;
		case 1: for(int i = cursorX; i >= 0; i--) {
				setChar(i, cursorY, ' ', attribute);
			}
			break;
		case 2: for(int i = 0; i <= width; i++) {
				setChar(i, cursorY, ' ', attribute);
			}
			break;
		default:;
		}
		break;
	}
	case 'm': {
		if(!params.empty())
			params.push_back(0);

		for(size_t i = 0; i < params.size(); i++) {
			int n = params[i];
			switch(n) {
				case 30:
					attribute.fgColor = kColorBlack;
					break;
				case 31:
					attribute.fgColor = kColorRed;
					break;
				case 32:
					attribute.fgColor = kColorGreen;
					break;
				case 33:
					attribute.fgColor = kColorYellow;
					break;
				case 34:
					attribute.fgColor = kColorBlue;
					break;
				case 35:
					attribute.fgColor = kColorMagenta;
					break;
				case 36:
					attribute.fgColor = kColorCyan;
					break;
				case 37:
					attribute.fgColor = kColorWhite;
					break;
				case 40:
					attribute.bgColor = kColorBlack;
					break;
				case 41:
					attribute.bgColor = kColorRed;
					break;
				case 42:
					attribute.bgColor = kColorGreen;
					break;
				case 43:
					attribute.bgColor = kColorYellow;
					break;
				case 44:
					attribute.bgColor = kColorBlue;
					break;
				case 45:
					attribute.bgColor = kColorMagenta;
					break;
				case 46:
					attribute.bgColor = kColorCyan;
					break;
				case 47:
					attribute.bgColor = kColorWhite;
					break;
			}
		}
	}
}

void Emulator::handleCsi(char character) {
	if(character >= '0' && character <= '9') {
		if(currentNumber) {
			currentNumber = *currentNumber * 10 + (character - '0');
		}else{
			currentNumber = character - '0';
		}
	}else if(character == ';') {
		if(currentNumber) {
			params.push_back(*currentNumber);
			currentNumber = std::experimental::nullopt;
		}else{
			params.push_back(0);
		}
	}else if(character >= 64 && character <= 126) {
		if(currentNumber) {
			params.push_back(*currentNumber);
			currentNumber = std::experimental::nullopt;
		}

		handleControlSeq(character);

		params.clear();
		status = kStatusNormal;
	}
}

void Emulator::printChar(char character) {
	switch (status) {
	if(status == kStatusNormal) {
		switch(character) {
		case 27: //ASCII for escape
			status = kStatusEscape;
			return;
		case '\a':
			// do nothing for now
			break;
		case '\b':
			if(cursorX > 0)
				cursorX--;
			break;
		case '\n':
			cursorY++;
			cursorX = 0;
			break;
		default: {
			Attribute attribute;
			setChar(cursorX, cursorY, character, attribute);
			cursorX++;
			if(cursorX >= width) {
				cursorX = 0;
				cursorY++;
			}
		}}
		if(cursorY >= height) {
			for(int i = 1; i < height; i++) {
				for(int j = 0; j < width; j++) {
					char moved_char = chars[j * width + width];
					Attribute moved_attribute = attributes[j * width + width];
					setChar(j, i - 1, moved_char, moved_attribute);
				}
			}
			for(int j = 0; j < width; j++) {
				Attribute attribute;
				attribute.fgColor = kColorWhite;
				setChar(j, height - 1, ' ', attribute);
			}
			cursorY = height - 1;
		}
		display->setCursor(cursorX, cursorY);
		break;
	}
	case kStatusEscape) {
		if(character == '[') {
			status = kStatusCsi;
		}
		break;
	}
	case kStatusCsi: {
		handleCsi(character);
	}
	default:;
	}
}

void Emulator::printString(const std::string& text){
	char buffer[8];
	for(char c : text) {
		if(logSequences) {
			sprintf(buffer, "U+%d", c);
			puts(buffer);
		}
		printChar(c);
	}
}

} // namespace libterminal
