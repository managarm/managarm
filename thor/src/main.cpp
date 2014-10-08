
extern "C" void thorRtMain();

namespace thor {
namespace io {

class VgaConsole {
public:
	VgaConsole(char *pointer, int width, int height)
			: p_pointer(pointer), p_width(width), p_height(height),
			p_cursorX(0), p_cursorY(0) { }

	void setCursor(int x, int y) {
		p_cursorX = x;
		p_cursorY = y;
	}

	void setChar(char c) {
		char *ptr = p_pointer + ((p_width * p_cursorY + p_cursorX) * 2);
		ptr[0] = c;
		ptr[1] = 0x0F;
	}

	void printChar(char c) {
		setChar(c);
		p_cursorX++;
	}
	void printString(const char *string) {
		while(*string != 0) {
			printChar(*string);
			string++;
		}
	}

	void clear() {
		for(int x = 0; x < p_width; x++)
			for(int y = 0; y < p_height; y++) {
				setCursor(x, y);
				setChar(' ');
			}
		
		setCursor(0, 0);
	}

private:
	char *p_pointer;
	int p_width;
	int p_height;
	int p_cursorX;
	int p_cursorY;
};

}};

void thorRtMain() {
	thor::io::VgaConsole console((char *)0xB8000, 80, 25);
	console.clear();
	console.printString("Starting Thor");
}

