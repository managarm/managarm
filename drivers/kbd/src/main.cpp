
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <helx.hpp>

#include <frigg/arch_x86/machine.hpp>
#include <bragi/mbus.hpp>
#include <input.pb.h>
#include <libchain/all.hpp>

helx::EventHub eventHub = helx::EventHub::create();
bragi_mbus::Connection mbusConnection(eventHub);
helx::Irq kbdIrq, mouseIrq;

// --------------------------------------------------------
// Mouse
// --------------------------------------------------------

enum MouseState {
	kMouseData,
	kMouseWaitForAck
};

enum MouseByte {
	kMouseByte0,
	kMouseByte1,
	kMouseByte2
};

MouseState mouseState = kMouseData;
MouseByte mouseByte = kMouseByte0;
uint8_t byte0 = 0;
uint8_t byte1 = 0;

std::vector<helx::Pipe> mouseServerPipes;

void handleMouseData(uint8_t data) {
	if(mouseState == kMouseWaitForAck) {
		assert(data == 0xFA);
		mouseState = kMouseData;
	}else{
		assert(mouseState == kMouseData);

		if(mouseByte == kMouseByte0) {
			if(data == 0xFA) { // acknowledge
				// do nothing for now
			}else{
				byte0 = data;
				assert(byte0 & 8);
				mouseByte = kMouseByte1;
			}
		}else if(mouseByte == kMouseByte1) {
			byte1 = data;
			mouseByte = kMouseByte2;
		}else{
			assert(mouseByte == kMouseByte2);
			uint8_t byte2 = data;
			if(byte0 & 4) {
				printf("MMB\n");
			}
			if(byte0 & 2) {
				printf("RMB\n");
			}
			if(byte0 & 1) {
				printf("LMB\n");
			}
			
			int movement_x = (byte0 & 16) ? -(256 - byte1) : byte1;
			int movement_y = (byte0 & 32) ? 256 - byte2 : -byte2;
			
			for(unsigned int i = 0; i < mouseServerPipes.size(); i++) {
				managarm::input::ServerRequest request;
				request.set_request_type(managarm::input::RequestType::MOVE);
				request.set_x(movement_x);
				request.set_y(movement_y);

				std::string serialized;
				request.SerializeToString(&serialized);

				printf("[drivers/kbd/src/main] handleMouseData sendStringReq\n");
				auto action = mouseServerPipes[i].sendStringReq(serialized.data(), serialized.size(),
					eventHub, 0, 0)
				+ libchain::lift([=] (HelError error) { HEL_CHECK(error); });
				libchain::run(frigg::move(action)); 
			}
			
			mouseByte = kMouseByte0;
		}
	}
}

// --------------------------------------------------------
// Keyboard
// --------------------------------------------------------

enum KeyboardStatus {
	kStatusNormal,
	kStatusE0,
	kStatusE1First,
	kStatusE1Second
};

KeyboardStatus escapeStatus = kStatusNormal;
uint8_t e1Buffer;

// numlock and capslock state
bool numState, capsState;

std::vector<helx::Pipe> kbdServerPipes;

void updateLed() {
	uint8_t led = 0;
	if(numState)
		led |= 1;
	if(capsState)
		led |= 2;

	frigg::arch_x86::ioOutByte(0x60, 0xED);
	while(!(frigg::arch_x86::ioInByte(0x60) == 0xFA)) {	};
	frigg::arch_x86::ioOutByte(0x60, led);
}

std::string scanNormal(uint8_t data) {
	switch(data) {
		case 0x01: return "Escape";
		case 0x02: return "Digit1";
		case 0x03: return "Digit2";
		case 0x04: return "Digit3";
		case 0x05: return "Digit4";
		case 0x06: return "Digit5";
		case 0x07: return "Digit6";
		case 0x08: return "Digit7";
		case 0x09: return "Digit8";
		case 0x0A: return "Digit9";
		case 0x0B: return "Digit0";
		case 0x0C: return "Minus";
		case 0x0D: return "Equal";
		case 0x0E: return "Backspace";
		case 0x0F: return "Tab";
		case 0x10: return "KeyQ";
		case 0x11: return "KeyW";
		case 0x12: return "KeyE";
		case 0x13: return "KeyR";
		case 0x14: return "KeyT";
		case 0x15: return "KeyY";
		case 0x16: return "KeyU";
		case 0x17: return "KeyI";
		case 0x18: return "KeyO";
		case 0x19: return "KeyP";
		case 0x1A: return "BracketLeft";
		case 0x1B: return "BracketRight";
		case 0x1C: return "Enter";
		case 0x1D: return "ControlLeft";
		case 0x1E: return "KeyA";
		case 0x1F: return "KeyS";
		case 0x20: return "KeyD";
		case 0x21: return "KeyF";
		case 0x22: return "KeyG";
		case 0x23: return "KeyH";
		case 0x24: return "KeyJ";
		case 0x25: return "KeyK";
		case 0x26: return "KeyL";
		case 0x27: return "Semicolon";
		case 0x28: return "Quote";
		case 0x29: return "Backquote";
		case 0x2A: return "ShiftLeft";
		case 0x2B: return "IntlHash";
		case 0x2C: return "KeyZ";
		case 0x2D: return "KeyX";
		case 0x2E: return "KeyC";
		case 0x2F: return "KeyV";
		case 0x30: return "KeyB";
		case 0x31: return "KeyN";
		case 0x32: return "KeyM";
		case 0x33: return "Comma";
		case 0x34: return "Period";
		case 0x35: return "Slash";
		case 0x36: return "ShiftRight";
		case 0x37: return "NumpadMultiply";
		case 0x38: return "AltLeft";
		case 0x39: return "Space";
		case 0x3A: return "CapsLock";
		case 0x3B: return "F1";
		case 0x3C: return "F2";
		case 0x3D: return "F3";
		case 0x3E: return "F4";
		case 0x3F: return "F5";
		case 0x40: return "F6";
		case 0x41: return "F7";
		case 0x42: return "F8";
		case 0x43: return "F9";
		case 0x44: return "F10";
		case 0x45: return "NumLock";
		case 0x46: return "ScrollLock";
		case 0x47: return "Numpad7";
		case 0x48: return "Numpad8";
		case 0x49: return "Numpad9";
		case 0x4A: return "NumpadSubtract";
		case 0x4B: return "Numpad4";
		case 0x4C: return "Numpad5";
		case 0x4D: return "Numpad6";
		case 0x4E: return "NumpadAdd";
		case 0x4F: return "Numpad1";
		case 0x50: return "Numpad2";
		case 0x51: return "Numpad3";
		case 0x52: return "Numpad0";
		case 0x53: return "NumpadDecimal";
		case 0x56: return "IntlBackslash";
		case 0x57: return "F11";
		case 0x58: return "F12";
		default: return "Unknown";
	}
}

std::string scanE0(uint8_t data) {
	switch(data) {
	case 0x1C: return "NumpadEnter";
	case 0x1D: return "ControlRight";
	case 0x35: return "NumpadDivide";
	case 0x37: return "PrintScreen";
	case 0x38: return "AltRight";
	case 0x47: return "Home";
	case 0x48: return "ArrowUp";
	case 0x49: return "PageUp";
	case 0x4B: return "ArrowLeft";
	case 0x4D: return "ArrowRight";
	case 0x4F: return "End";
	case 0x50: return "ArrowDown";
	case 0x51: return "PageDown";
	case 0x52: return "Insert";
	case 0x53: return "Delete";
	case 0x5B: return "OSLeft";
	case 0x5C: return "OSRight";
	case 0x5D: return "ContextMenu";
	default: return "Unknown";
	}
}

std::string scanE1(uint8_t data1, uint8_t data2) {
	if((data1 & 0x7F) == 0x1D && (data2 & 0X7F) == 0x45){
		return "Pause";
	}else{
		return "Unknown";
	}
}

void keyAction(std::string code, bool pressed) {
	if(pressed && code == "NumLock") {
		numState = !numState;

		for(unsigned int i = 0; i < kbdServerPipes.size(); i++) {
			auto action = libchain::compose([=] (std::string *serialized) {
				managarm::input::ServerRequest request;
				request.set_request_type(managarm::input::RequestType::CHANGE_STATE);
				request.set_state(numState);
				request.set_code(code);
			
				request.SerializeToString(serialized);

				printf("[drivers/kbd/src/main] keyAction sendStringReq1\n");
				return kbdServerPipes[i].sendStringReq(serialized->data(), serialized->size(),
						eventHub, 0, 0)
				+ libchain::lift([=] (HelError error) { HEL_CHECK(error); });
			}, std::string());
			libchain::run(frigg::move(action));
		}
	}else if(pressed && code == "CapsLock") {
		capsState = !capsState;

		for(unsigned int i = 0; i < kbdServerPipes.size(); i++) {
			auto action = libchain::compose([=] (std::string *serialized) {
				managarm::input::ServerRequest request;
				request.set_request_type(managarm::input::RequestType::CHANGE_STATE);
				request.set_state(capsState);
				request.set_code(code);
			
				request.SerializeToString(serialized);
				
				printf("[drivers/kbd/src/main] keyAction sendStringReq2\n");
				return kbdServerPipes[i].sendStringReq(serialized->data(), serialized->size(),
						eventHub, 0, 0)
				+ libchain::lift([=] (HelError error) { HEL_CHECK(error); });
			}, std::string());
			libchain::run(frigg::move(action));
		}
	}
	
	for(unsigned int i = 0; i < kbdServerPipes.size(); i++) {
		auto action = libchain::compose([=] (std::string *serialized) {
			managarm::input::ServerRequest request;
			
			if(pressed) {
				request.set_request_type(managarm::input::RequestType::DOWN);
			}else{	
				request.set_request_type(managarm::input::RequestType::UP);
			}
			request.set_code(code);
			
			request.SerializeToString(serialized);
			
			return kbdServerPipes[i].sendStringReq(serialized->data(), serialized->size(),
					eventHub, 0, 0)
			+ libchain::lift([=] (HelError error) { HEL_CHECK(error); });
		}, std::string());
		libchain::run(frigg::move(action));
	}

	//updateLed();
}

void handleKeyboardData(uint8_t data) {
	std::string code;
	if(escapeStatus == kStatusE1First) {
		e1Buffer = data;

		escapeStatus = kStatusE1Second;
	}else if(escapeStatus == kStatusE1Second) {
		assert((e1Buffer & 0x80) == (data & 0x80));
		keyAction(scanE1(e1Buffer & 0x7F, data & 0x7F), !(data & 0x80));

		escapeStatus = kStatusNormal;
	}else if(escapeStatus == kStatusE0) {
		keyAction(scanE0(data & 0x7F), !(data & 0x80));

		escapeStatus = kStatusNormal;
	}else{
		assert(escapeStatus == kStatusNormal);
	
		if(data == 0xE0) {
			escapeStatus = kStatusE0;
			return;
		}else if(data == 0xE1) {
			escapeStatus = kStatusE1First;
			return;
		}
		
		keyAction(scanNormal(data & 0x7F), !(data & 0x80));
	}
	
}

// --------------------------------------------------------
// Controller
// --------------------------------------------------------

void sendByte(uint8_t data) {
	frigg::arch_x86::ioOutByte(0x64, 0xD4);
	while(frigg::arch_x86::ioInByte(0x64) & 0x02) { };
	frigg::arch_x86::ioOutByte(0x60, data);
}

void readDeviceData() {
	uint8_t status = frigg::arch_x86::ioInByte(0x64);
	if(!(status & 0x01))
		return;
	
	uint8_t data = frigg::arch_x86::ioInByte(0x60);
	if(status & 0x20) {
		handleMouseData(data);
	}else{
		handleKeyboardData(data);
	}
}

void onMouseInterrupt(void * object, HelError error) {
	HEL_CHECK(error);

	readDeviceData();

//	HEL_CHECK(helAcknowledgeIrq(mouseIrq.getHandle()));
	mouseIrq.wait(eventHub, CALLBACK_STATIC(nullptr, &onMouseInterrupt));
}

void onKbdInterrupt(void * object, HelError error) {
	HEL_CHECK(error);

	readDeviceData();

//	HEL_CHECK(helAcknowledgeIrq(kbdIrq.getHandle()));
	kbdIrq.wait(eventHub, CALLBACK_STATIC(nullptr, &onKbdInterrupt));
}

// --------------------------------------------------------
// ObjectHandler
// --------------------------------------------------------

bragi_mbus::ObjectId kbdObjectId;
bragi_mbus::ObjectId mouseObjectId;

struct ObjectHandler : public bragi_mbus::ObjectHandler {
	// inherited from bragi_mbus::ObjectHandler
	void requireIf(bragi_mbus::ObjectId object_id,
			frigg::CallbackPtr<void(HelHandle)> callback);
};

void ObjectHandler::requireIf(bragi_mbus::ObjectId object_id,
		frigg::CallbackPtr<void(HelHandle)> callback) {
	helx::Pipe server_side, client_side;
	helx::Pipe::createFullPipe(server_side, client_side);
	callback(client_side.getHandle());
	client_side.reset();

	if(object_id == kbdObjectId) {
		kbdServerPipes.push_back(std::move(server_side));
	}else{
		assert(object_id == mouseObjectId);
		mouseServerPipes.push_back(std::move(server_side));
		printf("mouse client connected\n");
	}
}

ObjectHandler objectHandler;

// --------------------------------------------------------
// InitClosure
// --------------------------------------------------------

struct InitClosure : public frigg::BaseClosure<InitClosure> {
	void operator() ();

private:
	void connected();
	void registeredKbd(bragi_mbus::ObjectId object_id);
	void registeredMouse(bragi_mbus::ObjectId object_id);
};

void InitClosure::operator() () {
	mbusConnection.connect(CALLBACK_MEMBER(this, &InitClosure::connected));
}

void InitClosure::connected() {
	mbusConnection.registerObject("keyboard",
			CALLBACK_MEMBER(this, &InitClosure::registeredKbd));
	mbusConnection.registerObject("mouse",
			CALLBACK_MEMBER(this, &InitClosure::registeredMouse));
}

void InitClosure::registeredKbd(bragi_mbus::ObjectId object_id) {
	kbdObjectId = object_id;
}

void InitClosure::registeredMouse(bragi_mbus::ObjectId object_id) {
	mouseObjectId = object_id;
}

int main() {
	printf("Starting ps/2\n");
	kbdIrq = helx::Irq::access(1);
	mouseIrq = helx::Irq::access(12);

//	HEL_CHECK(helSetupIrq(kbdIrq.getHandle(), kHelIrqExclusive | kHelIrqManualAcknowledge));
//	HEL_CHECK(helSetupIrq(mouseIrq.getHandle(), kHelIrqExclusive | kHelIrqManualAcknowledge));
	
	uintptr_t ports[] = { 0x60, 0x64 };
	HelHandle handle;
	HEL_CHECK(helAccessIo(ports, 2, &handle));
	HEL_CHECK(helEnableIo(handle));

	kbdIrq.wait(eventHub, CALLBACK_STATIC(nullptr, &onKbdInterrupt));
	mouseIrq.wait(eventHub, CALLBACK_STATIC(nullptr, &onMouseInterrupt));

	// disable both devices
	frigg::arch_x86::ioOutByte(0x64, 0xAD);
	frigg::arch_x86::ioOutByte(0x64, 0xA7);

	// flush the output buffer
	while(frigg::arch_x86::ioInByte(0x64) & 0x01)
		frigg::arch_x86::ioInByte(0x60);

	// enable interrupt for second device
	frigg::arch_x86::ioOutByte(0x64, 0x20);
	uint8_t configuration = frigg::arch_x86::ioInByte(0x60);
	configuration |= 0x02;
	frigg::arch_x86::ioOutByte(0x64, 0x60);
	frigg::arch_x86::ioOutByte(0x60, configuration);

	// enable both devices
	frigg::arch_x86::ioOutByte(0x64, 0xAE);
	frigg::arch_x86::ioOutByte(0x64, 0xA8);

	// enables mouse response
	sendByte(0xF4);
	mouseState = kMouseWaitForAck;

	mbusConnection.setObjectHandler(&objectHandler);
	auto closure = new InitClosure();
	(*closure)();
	
	pid_t child = fork();
	assert(child != -1);
	if(!child) {
//		execve("/usr/bin/bochs_vga", nullptr, nullptr);
		execve("/usr/bin/vga_terminal", nullptr, nullptr);
	}
	
	while(true) {
		eventHub.defaultProcessEvents();
	}
}

