
namespace thor {
namespace debug {

void panic();

#define ASSERT(c) do { if(!(c)) ::thor::debug::assertionFail(#c); } while(0)
#define ASSERT_UNREACHABLE() do { ::thor::debug::assertionFail("unreachable"); } while(0)

class Screen {
public:
	virtual int getWidth() = 0;
	virtual int getHeight() = 0;
	
	virtual int getCursorX() = 0;
	virtual int getCursorY() = 0;

	virtual void setCursor(int x, int y) = 0;
	virtual void setChar(char c) = 0;
};

class VgaScreen : public Screen {
public:
	VgaScreen(char *pointer, int width, int height);
	
	virtual int getWidth();
	virtual int getHeight();

	virtual int getCursorX();
	virtual int getCursorY();

	virtual void setCursor(int x, int y);
	virtual void setChar(char c);

private:
	char *p_pointer;
	int p_width;
	int p_height;
	int p_cursorX;
	int p_cursorY;
};

class Terminal {
public:
	Terminal(Screen *screen);

	void advanceCursor();

	void printChar(char c);

	void clear();

private:
	Screen *p_screen;
};

class Logger {
public:
	virtual void print(char c) = 0;
	
	void log(const char *string);
	void log(const char *string, size_t length);
	void log(void *pointer);
	void log(int number);
	void logHex(int number);

	template<typename T>
	void logUInt(T number, int radix);
};

class TerminalLogger : public Logger {
public:
	TerminalLogger(Terminal *terminal);

	virtual void print(char c);

private:
	Terminal *p_terminal;
};

extern Logger *criticalLogger;

// --------------------------------------------------------
// Logger inline functions
// --------------------------------------------------------

template<typename T>
void Logger::logUInt(T number, int radix) {
	if(number == 0) {
		print('0');
		return;
	}
	const char *digits = "0123456789abcdef";
	int logarithm = 0;
	T rem = number;
	while(1) {
		rem /= radix;
		if(rem == 0)
			break;
		logarithm++;
	}
	T p = 1;
	for(int i = 0; i < logarithm; i++)
		p *= radix;
	while(p > 0) {
		int d = number / p;
		print(digits[d]);
		number %= p;
		p /= radix;
	}
}

void assertionFail(const char *message);

}} // namespace thor::debug

