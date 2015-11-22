
#include <string>

enum KeyType {
	kKeyChars,
	kKeySpecial
};

struct ComposeHandler {
	virtual void input(std::string string) = 0;
};

struct ComposeState {
	ComposeState(ComposeHandler *handler);

	void keyPress(std::pair<KeyType, std::string> value);

private:
	ComposeHandler *handler;
};

struct Translator {
	std::pair<KeyType, std::string> translate(std::string code);
	void keyUp(std::string code);
	void keyDown(std::string code);

private:
	bool shift;
	bool altgr;
};

