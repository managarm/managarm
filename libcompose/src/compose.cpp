
#include <assert.h>
#include <libcompose.hpp>

ComposeState::ComposeState(ComposeHandler *handler) : handler(handler) { }

std::pair<KeyType, std::string> translate(std::string code, bool shift, bool altgr) {
	if(shift) {
		if(code == "KeyQ") return { kKeyChars, "Q" };
		if(code == "KeyW") return { kKeyChars, "W" };
		if(code == "KeyE") return { kKeyChars, "E" };
		if(code == "KeyR") return { kKeyChars, "R" };
		if(code == "KeyT") return { kKeyChars, "T" };
		if(code == "KeyY") return { kKeyChars, "Z" };
		if(code == "KeyU") return { kKeyChars, "U" };
		if(code == "KeyI") return { kKeyChars, "I" };
		if(code == "KeyO") return { kKeyChars, "O" };
		if(code == "KeyP") return { kKeyChars, "P" };
		if(code == "KeyA") return { kKeyChars, "A" };
		if(code == "KeyS") return { kKeyChars, "S" };
		if(code == "KeyD") return { kKeyChars, "D" };
		if(code == "KeyF") return { kKeyChars, "F" };
		if(code == "KeyG") return { kKeyChars, "G" };
		if(code == "KeyH") return { kKeyChars, "H" };
		if(code == "KeyJ") return { kKeyChars, "J" };
		if(code == "KeyK") return { kKeyChars, "K" };
		if(code == "KeyL") return { kKeyChars, "L" };
		if(code == "KeyZ") return { kKeyChars, "Y" };
		if(code == "KeyX") return { kKeyChars, "X" };
		if(code == "KeyC") return { kKeyChars, "C" };
		if(code == "KeyV") return { kKeyChars, "V" };
		if(code == "KeyB") return { kKeyChars, "B" };
		if(code == "KeyN") return { kKeyChars, "N" };
		if(code == "KeyM") return { kKeyChars, "M" };

		if(code == "Backquote") return { kKeyChars, "°" };
		if(code == "Digit1") return { kKeyChars, "!" };
		if(code == "Digit2") return { kKeyChars, "\"" };
		if(code == "Digit3") return { kKeyChars, "§" };
		if(code == "Digit4") return { kKeyChars, "$" };
		if(code == "Digit5") return { kKeyChars, "%" };
		if(code == "Digit6") return { kKeyChars, "&" };
		if(code == "Digit7") return { kKeyChars, "/" };
		if(code == "Digit8") return { kKeyChars, "(" };
		if(code == "Digit9") return { kKeyChars, ")" };
		if(code == "Digit0") return { kKeyChars, "=" };
		if(code == "Minus") return { kKeyChars, "?" };
		if(code == "Equal") return { kKeyChars, "`" };

		if(code == "BracketLeft") return { kKeyChars, "Ü" };
		if(code == "BracketRight") return { kKeyChars, "*" };
		if(code == "Semicolon") return { kKeyChars, "Ö" };
		if(code == "Quote") return { kKeyChars, "Ä" };
		if(code == "IntlHash") return { kKeyChars, "'" };
		if(code == "IntlBackslash") return { kKeyChars, ">" };
		if(code == "Comma") return { kKeyChars, ";" };
		if(code == "Period") return { kKeyChars, ":" };
		if(code == "Slash") return { kKeyChars, "_" };
	}else if(altgr){
		if(code == "Digit2") return { kKeyChars, "²" };
		if(code == "Digit3") return { kKeyChars, "³" };
		if(code == "Digit7") return { kKeyChars, "{" };
		if(code == "Digit8") return { kKeyChars, "[" };
		if(code == "Digit9") return { kKeyChars, "]" };
		if(code == "Digit0") return { kKeyChars, "}" };
		if(code == "Minus") return { kKeyChars, "\\" };
		if(code == "KeyQ") return { kKeyChars, "@" };
		if(code == "KeyE") return { kKeyChars, "€" };
		if(code == "BracketRight") return { kKeyChars, "~" };
		if(code == "IntlBackslash") return { kKeyChars, "|" };
		if(code == "KeyM") return { kKeyChars, "µ" };
	}else{
		if(code == "KeyQ") return { kKeyChars, "q" };
		if(code == "KeyW") return { kKeyChars, "w" };
		if(code == "KeyE") return { kKeyChars, "e" };
		if(code == "KeyR") return { kKeyChars, "r" };
		if(code == "KeyT") return { kKeyChars, "t" };
		if(code == "KeyY") return { kKeyChars, "z" };
		if(code == "KeyU") return { kKeyChars, "u" };
		if(code == "KeyI") return { kKeyChars, "i" };
		if(code == "KeyO") return { kKeyChars, "o" };
		if(code == "KeyP") return { kKeyChars, "p" };
		if(code == "KeyA") return { kKeyChars, "a" };
		if(code == "KeyS") return { kKeyChars, "s" };
		if(code == "KeyD") return { kKeyChars, "d" };
		if(code == "KeyF") return { kKeyChars, "f" };
		if(code == "KeyG") return { kKeyChars, "g" };
		if(code == "KeyH") return { kKeyChars, "h" };
		if(code == "KeyJ") return { kKeyChars, "j" };
		if(code == "KeyK") return { kKeyChars, "k" };
		if(code == "KeyL") return { kKeyChars, "l" };
		if(code == "KeyZ") return { kKeyChars, "y" };
		if(code == "KeyX") return { kKeyChars, "x" };
		if(code == "KeyC") return { kKeyChars, "c" };
		if(code == "KeyV") return { kKeyChars, "v" };
		if(code == "KeyB") return { kKeyChars, "b" };
		if(code == "KeyN") return { kKeyChars, "n" };
		if(code == "KeyM") return { kKeyChars, "m" };

		if(code == "Backquote") return { kKeyChars, "^" };
		if(code == "Digit1") return { kKeyChars, "1" };
		if(code == "Digit2") return { kKeyChars, "2" };
		if(code == "Digit3") return { kKeyChars, "3" };
		if(code == "Digit4") return { kKeyChars, "4" };
		if(code == "Digit5") return { kKeyChars, "5" };
		if(code == "Digit6") return { kKeyChars, "6" };
		if(code == "Digit7") return { kKeyChars, "7" };
		if(code == "Digit8") return { kKeyChars, "8" };
		if(code == "Digit9") return { kKeyChars, "9" };
		if(code == "Digit0") return { kKeyChars, "0" };
		if(code == "Minus") return { kKeyChars, "ß" };
		if(code == "Equal") return { kKeyChars, "´" };

		if(code == "BracketLeft") return { kKeyChars, "ü" };
		if(code == "BracketRight") return { kKeyChars, "+" };
		if(code == "Semicolon") return { kKeyChars, "ö" };
		if(code == "Quote") return { kKeyChars, "ä" };
		if(code == "IntlHash") return { kKeyChars, "#" };
		if(code == "IntlBackslash") return { kKeyChars, "<" };
		if(code == "Comma") return { kKeyChars, "," };
		if(code == "Period") return { kKeyChars, "." };
		if(code == "Slash") return { kKeyChars, "-" };
	}
	
	if(code == "Numpad0") return { kKeyChars, "0" };
	if(code == "Numpad1") return { kKeyChars, "1" };
	if(code == "Numpad2") return { kKeyChars, "2" };
	if(code == "Numpad3") return { kKeyChars, "3" };
	if(code == "Numpad4") return { kKeyChars, "4" };
	if(code == "Numpad5") return { kKeyChars, "5" };
	if(code == "Numpad6") return { kKeyChars, "6" };
	if(code == "Numpad7") return { kKeyChars, "7" };
	if(code == "Numpad8") return { kKeyChars, "8" };
	if(code == "Numpad9") return { kKeyChars, "9" };
	if(code == "NumpadDivide") return { kKeyChars, "/" };
	if(code == "NumpadMultiply") return { kKeyChars, "*" };
	if(code == "NumpadSubtract") return { kKeyChars, "-" };
	if(code == "NumpadAdd") return { kKeyChars, "+" };
	if(code == "NumpadDecimal") return { kKeyChars, "," };


	if(code == "AltLeft") return { kKeySpecial, "Alt" };
	if(code == "AltRight") return { kKeySpecial, "AltGraph" };
	if(code == "CapsLock") return { kKeySpecial, "CapsLock" };
	if(code == "ControlLeft") return { kKeySpecial, "Contol" };
	if(code == "ControlRight") return { kKeySpecial, "Contol" };
	if(code == "NumLock") return { kKeySpecial, "NumLock" };
	if(code == "OSLeft") return { kKeySpecial, "OS" };
	if(code == "OSRight") return { kKeySpecial, "OS" };
	if(code == "ScrollLock") return { kKeySpecial, "ScrollLock" };
	if(code == "ShiftLeft") return { kKeySpecial, "Shift" };
	if(code == "ShiftRight") return { kKeySpecial, "Shift" };
	if(code == "Enter") return { kKeySpecial, "Enter" };
	if(code == "NumpadEnter") return { kKeySpecial, "Enter" };
	if(code == "Tab") return { kKeySpecial, "Tab" };
	if(code == "Space") return { kKeyChars, " " };
	if(code == "ArrowLeft") return { kKeySpecial, "ArrowLeft" };
	if(code == "ArrowDown") return { kKeySpecial, "ArrowDown" };
	if(code == "ArrowRight") return { kKeySpecial, "ArrowRight" };
	if(code == "ArrowUp") return { kKeySpecial, "ArrowUp" };
	if(code == "End") return { kKeySpecial, "End" };
	if(code == "Home") return { kKeySpecial, "Home" };
	if(code == "PageDown") return { kKeySpecial, "PageDown" };
	if(code == "PageUp") return { kKeySpecial, "PageUp" };
	if(code == "Backspace") return { kKeySpecial, "Backspace" };
	if(code == "Delete") return { kKeySpecial, "Delete" };
	if(code == "Insert") return { kKeySpecial, "Insert" };
	if(code == "ArrowUp") return { kKeySpecial, "Shift" };
	if(code == "ContextMenu") return { kKeySpecial, "ContextMenu" };
	if(code == "Escape") return { kKeySpecial, "Escape" };
	if(code == "PrintScreen") return { kKeySpecial, "PrintScreen" };
	if(code == "Pause") return { kKeySpecial, "Pause" };
	if(code == "F1") return { kKeySpecial, "F1" };
	if(code == "F2") return { kKeySpecial, "F2" };
	if(code == "F3") return { kKeySpecial, "F3" };
	if(code == "F4") return { kKeySpecial, "F4" };
	if(code == "F5") return { kKeySpecial, "F5" };
	if(code == "F6") return { kKeySpecial, "F6" };
	if(code == "F7") return { kKeySpecial, "F7" };
	if(code == "F8") return { kKeySpecial, "F8" };
	if(code == "F9") return { kKeySpecial, "F9" };
	if(code == "F10") return { kKeySpecial, "F10" };
	if(code == "F11") return { kKeySpecial, "F11" };
	if(code == "F12") return { kKeySpecial, "F12" };

	return { kKeySpecial, "Unidentified" };
}

void ComposeState::keyPress(std::pair<KeyType, std::string> value) {
	if(value.first == kKeyChars) {
		handler->input(value.second);
	}else if(value.first == kKeySpecial) {
		if(value.second == "Enter") {
			handler->input("\n");
		}else if(value.second == "Tab") {
			handler->input("\t");
		}
	}else {
		assert(!"No matched Key");
	}
 }

