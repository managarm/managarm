
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <string>

#include <helx.hpp>

#include <frigg/arch_x86/machine.hpp>

helx::EventHub eventHub = helx::EventHub::create();
helx::Irq irq;
bool shift;

std::string translate(std::string code) {
	if(shift) {
		if(code == "KeyQ") return "Q";
		if(code == "KeyW") return "W";
		if(code == "KeyE") return "E";
		if(code == "KeyR") return "R";
		if(code == "KeyT") return "T";
		if(code == "KeyY") return "Z";
		if(code == "KeyU") return "U";
		if(code == "KeyI") return "I";
		if(code == "KeyO") return "O";
		if(code == "KeyP") return "P";
		if(code == "KeyA") return "A";
		if(code == "KeyS") return "S";
		if(code == "KeyD") return "D";
		if(code == "KeyF") return "F";
		if(code == "KeyG") return "G";
		if(code == "KeyH") return "H";
		if(code == "KeyJ") return "J";
		if(code == "KeyK") return "K";
		if(code == "KeyL") return "L";
		if(code == "KeyZ") return "Y";
		if(code == "KeyX") return "X";
		if(code == "KeyC") return "C";
		if(code == "KeyV") return "V";
		if(code == "KeyB") return "B";
		if(code == "KeyN") return "N";
		if(code == "KeyM") return "M";

		if(code == "Digit1") return "!";
		if(code == "Digit2") return "\"";
		if(code == "Digit3") return "§";
		if(code == "Digit4") return "$";
		if(code == "Digit5") return "%";
		if(code == "Digit6") return "&";
		if(code == "Digit7") return "/";
		if(code == "Digit8") return "(";
		if(code == "Digit9") return ")";
		if(code == "Digit0") return "=";
		if(code == "Minus") return "?";
		if(code == "Equal") return "`";

		if(code == "BracketLeft") return "Ü";
		if(code == "BracketRight") return "*";
		if(code == "Semicolon") return "Ö";
		if(code == "Quote") return "Ä";
		if(code == "Comma") return ";";
		if(code == "Period") return ":";
		if(code == "Slash") return "_";
	}else{
		if(code == "KeyQ") return "q";
		if(code == "KeyW") return "w";
		if(code == "KeyE") return "e";
		if(code == "KeyR") return "r";
		if(code == "KeyT") return "t";
		if(code == "KeyY") return "z";
		if(code == "KeyU") return "u";
		if(code == "KeyI") return "i";
		if(code == "KeyO") return "o";
		if(code == "KeyP") return "p";
		if(code == "KeyA") return "a";
		if(code == "KeyS") return "s";
		if(code == "KeyD") return "d";
		if(code == "KeyF") return "f";
		if(code == "KeyG") return "g";
		if(code == "KeyH") return "h";
		if(code == "KeyJ") return "j";
		if(code == "KeyK") return "k";
		if(code == "KeyL") return "l";
		if(code == "KeyZ") return "y";
		if(code == "KeyX") return "x";
		if(code == "KeyC") return "c";
		if(code == "KeyV") return "v";
		if(code == "KeyB") return "b";
		if(code == "KeyN") return "n";
		if(code == "KeyM") return "m";

		if(code == "Digit1") return "1";
		if(code == "Digit2") return "2";
		if(code == "Digit3") return "3";
		if(code == "Digit4") return "4";
		if(code == "Digit5") return "5";
		if(code == "Digit6") return "6";
		if(code == "Digit7") return "7";
		if(code == "Digit8") return "8";
		if(code == "Digit9") return "9";
		if(code == "Digit0") return "0";
		if(code == "Minus") return "ß";
		if(code == "Equal") return "´";

		if(code == "BracketLeft") return "ü";
		if(code == "BracketRight") return "+";
		if(code == "Semicolon") return "ö";
		if(code == "Quote") return "ä";
		if(code == "Comma") return ",";
		if(code == "Period") return ".";
		if(code == "Slash") return "-";
	}
	
	if(code == "NumpadMultiply") return "*";
	if(code == "NumpadSubtract") return "-";
	if(code == "NumpadAdd") return "+";
	if(code == "NumpadDecimal") return ",";

	if(code == "Backspace") return "Backspace";

	return "Unidentified";
}

void onInterrupt(void * object, HelError error) {
	HEL_CHECK(error);

	std::string code;

	while(true) {
		uint8_t status = frigg::arch_x86::ioInByte(0x64);
		if(!(status & 0x01))
			break;

		uint8_t scan_code = frigg::arch_x86::ioInByte(0x60);

		switch(scan_code & 0x7F) {
			case 0x01: code = "Escape"; break;
			case 0x02: code = "Digit1"; break;
			case 0x03: code = "Digit2"; break;
			case 0x04: code = "Digit3"; break;
			case 0x05: code = "Digit4"; break;
			case 0x06: code = "Digit5"; break;
			case 0x07: code = "Digit6"; break;
			case 0x08: code = "Digit7"; break;
			case 0x09: code = "Digit8"; break;
			case 0x0A: code = "Digit9"; break;
			case 0x0B: code = "Digit0"; break;
			case 0x0C: code = "Minus"; break;
			case 0x0D: code = "Equal"; break;
			case 0x0E: code = "Backspace"; break;
			case 0x0F: code = "Tab"; break;
			case 0x10: code = "KeyQ"; break;
			case 0x11: code = "KeyW"; break;
			case 0x12: code = "KeyE"; break;
			case 0x13: code = "KeyR"; break;
			case 0x14: code = "KeyT"; break;
			case 0x15: code = "KeyY"; break;
			case 0x16: code = "KeyU"; break;
			case 0x17: code = "KeyI"; break;
			case 0x18: code = "KeyO"; break;
			case 0x19: code = "KeyP"; break;
			case 0x1A: code = "BracketLeft"; break;
			case 0x1B: code = "BracketRight"; break;
			case 0x1C: code = "Enter"; break;
			case 0x1D: code = "ControlLeft"; break;
			case 0x1E: code = "KeyA"; break;
			case 0x1F: code = "KeyS"; break;
			case 0x20: code = "KeyD"; break;
			case 0x21: code = "KeyF"; break;
			case 0x22: code = "KeyG"; break;
			case 0x23: code = "KeyH"; break;
			case 0x24: code = "KeyJ"; break;
			case 0x25: code = "KeyK"; break;
			case 0x26: code = "KeyL"; break;
			case 0x27: code = "Semicolon"; break;
			case 0x28: code = "Quote"; break;
			//case 0x29: code = ""; break;
			case 0x2A: code = "ShiftLeft"; break;
			case 0x2B: code = "IntlBackslash"; break;
			case 0x2C: code = "KeyZ"; break;
			case 0x2D: code = "KeyX"; break;
			case 0x2E: code = "KeyC"; break;
			case 0x2F: code = "KeyV"; break;
			case 0x30: code = "KeyB"; break;
			case 0x31: code = "KeyN"; break;
			case 0x32: code = "KeyM"; break;
			case 0x33: code = "Comma"; break;
			case 0x34: code = "Period"; break;
			case 0x35: code = "Slash"; break;
			case 0x36: code = "ShiftRight"; break;
			case 0x37: code = "NumpadMultiply"; break;
			case 0x38: code = "AltLeft"; break;
			case 0x39: code = "Space"; break;
			case 0x3A: code = "CapsLock"; break;
			case 0x3B: code = "F1"; break;
			case 0x3C: code = "F2"; break;
			case 0x3D: code = "F3"; break;
			case 0x3E: code = "F4"; break;
			case 0x3F: code = "F5"; break;
			case 0x40: code = "F6"; break;
			case 0x41: code = "F7"; break;
			case 0x42: code = "F8"; break;
			case 0x43: code = "F9"; break;
			case 0x44: code = "F10"; break;
			case 0x45: code = "NumLock"; break;
			case 0x46: code = "ScrollLock"; break;
			case 0x47: code = "Numpad7"; break;
			case 0x48: code = "Numpad8"; break;
			case 0x49: code = "Numpad9"; break;
			case 0x4A: code = "NumpadSubtract"; break;
			case 0x4B: code = "Numpad4"; break;
			case 0x4C: code = "Numpad5"; break;
			case 0x4D: code = "Numpad6"; break;
			case 0x4E: code = "NumpadAdd"; break;
			case 0x4F: code = "Numpad1"; break;
			case 0x50: code = "Numpad2"; break;
			case 0x51: code = "Numpad3"; break;
			case 0x52: code = "Numpad0"; break;
			case 0x53: code = "NumpadDecimal"; break;
			case 0x57: code = "F11"; break;
			case 0x58: code = "F12"; break;

			default: code = "Unknown"; break;
		}

		bool pressed = !(scan_code & 0x80);
		if(pressed) {
			if(code == "ShiftLeft" || code == "ShiftRight") {
				shift = true;
			}
		}else{
			if(code == "ShiftLeft" || code == "ShiftRight") {
				shift = false;
			}
		}

		std::string key = translate(code);
		printf("%s\n", key.data());
	}

	irq.wait(eventHub, CALLBACK_STATIC(nullptr, &onInterrupt));
}

#include <unistd.h>

int main() {
	printf("Starting kbd\n");
	irq = helx::Irq::access(1);
	
	uintptr_t ports[] = { 0x60, 0x64 };
	HelHandle handle;
	HEL_CHECK(helAccessIo(ports, 2, &handle));
	HEL_CHECK(helEnableIo(handle));

	irq.wait(eventHub, CALLBACK_STATIC(nullptr, &onInterrupt));
	
	pid_t child = fork();
	assert(child != -1);
	if(!child) {
		execve("vga_terminal", nullptr, nullptr);
	}

	while(true) {
		eventHub.defaultProcessEvents();
	}
}

