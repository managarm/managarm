
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

enum Status {
	kStatusNormal,
	kStatusE0,
	kStatusE1First,
	kStatusE1Second
};

helx::EventHub eventHub = helx::EventHub::create();
bragi_mbus::Connection mbusConnection(eventHub);
helx::Irq irq;
Status escapeStatus = kStatusNormal;
int firstScancode;
std::vector<helx::Pipe> serverPipes;
bool numState;
bool capsState;

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

void onInterrupt(void * object, HelError error) {
	HEL_CHECK(error);

	std::string code;

	while(true) {
		uint8_t status = frigg::arch_x86::ioInByte(0x64);
		if(!(status & 0x01))
			break;

		uint8_t scan_code = frigg::arch_x86::ioInByte(0x60);

		if(scan_code == 0xE0) {
			escapeStatus = kStatusE0;
			continue;
		}else if(scan_code == 0xE1) {
			escapeStatus = kStatusE1First;
			continue;
		}

		if(escapeStatus == kStatusE1First) {
			firstScancode = scan_code;
			escapeStatus = kStatusE1Second;
			continue;
		}else if(escapeStatus == kStatusE1Second) {
			if((firstScancode & 0x7F) == 0x1D && (scan_code & 0X7F) == 0x45){
				code = "Pause";
			}else{
				code = "Unknown";
			}

			escapeStatus = kStatusNormal;
		}else if(escapeStatus == kStatusE0) {
			switch(scan_code & 0x7F) {
				case 0x1C: code = "NumpadEnter"; break;
				case 0x1D: code = "ControlRight"; break;
				case 0x35: code = "NumpadDivide"; break;
				case 0x37: code = "PrintScreen"; break;
				case 0x38: code = "AltRight"; break;
				case 0x47: code = "Home"; break;
				case 0x48: code = "ArrowUp"; break;
				case 0x49: code = "PageUp"; break;
				case 0x4B: code = "ArrowLeft"; break;
				case 0x4D: code = "ArrowRight"; break;
				case 0x4F: code = "End"; break;
				case 0x50: code = "ArrowDown"; break;
				case 0x51: code = "PageDown"; break;
				case 0x52: code = "Insert"; break;
				case 0x53: code = "Delete"; break;
				case 0x5B: code = "OSLeft"; break;
				case 0x5C: code = "OSRight"; break;
				case 0x5D: code = "ContextMenu"; break;
				default: code = "Unknown"; break;
			}

			escapeStatus = kStatusNormal;
		}else{
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
				case 0x29: code = "Backquote"; break;
				case 0x2A: code = "ShiftLeft"; break;
				case 0x2B: code = "IntlHash"; break;
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
				case 0x56: code = "IntlBackslash"; break;
				case 0x57: code = "F11"; break;
				case 0x58: code = "F12"; break;
				default: code = "Unknown"; break;
			}
		}
		
		bool pressed = !(scan_code & 0x80);
		
		if(pressed && code == "NumLock") {
			numState = !numState;

			for(unsigned int i = 0; i < serverPipes.size(); i++) {
				managarm::input::ServerRequest stateRequest;
				stateRequest.set_request_type(managarm::input::RequestType::CHANGE_STATE);
				stateRequest.set_state(numState);
				stateRequest.set_code(code);
			
				std::string serialized;
				stateRequest.SerializeToString(&serialized);
				serverPipes[i].sendStringReq(serialized.data(), serialized.size() , 0, 0);
			}
		}else if(pressed && code == "CapsLock") {
			capsState = !capsState;

			for(unsigned int i = 0; i < serverPipes.size(); i++) {
				managarm::input::ServerRequest stateRequest;
				stateRequest.set_request_type(managarm::input::RequestType::CHANGE_STATE);
				stateRequest.set_state(capsState);
				stateRequest.set_code(code);
			
				std::string serialized;
				stateRequest.SerializeToString(&serialized);
				serverPipes[i].sendStringReq(serialized.data(), serialized.size() , 0, 0);
			}
		}
		
		for(unsigned int i = 0; i < serverPipes.size(); i++) {
			managarm::input::ServerRequest request;
			
			if(pressed) {
				request.set_request_type(managarm::input::RequestType::DOWN);
			}else{	
				request.set_request_type(managarm::input::RequestType::UP);
			}
			request.set_code(code);
			
			std::string serialized;
			request.SerializeToString(&serialized);
			serverPipes[i].sendStringReq(serialized.data(), 
					serialized.size() , 0, 0);
		}
	}

	//updateLed();

	irq.wait(eventHub, CALLBACK_STATIC(nullptr, &onInterrupt));
}

// --------------------------------------------------------
// ObjectHandler
// --------------------------------------------------------

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
	serverPipes.push_back(std::move(server_side));
}

ObjectHandler objectHandler;

// --------------------------------------------------------
// InitClosure
// --------------------------------------------------------

struct InitClosure : public frigg::BaseClosure<InitClosure> {
	void operator() ();

private:
	void connected();
	void registered(bragi_mbus::ObjectId object_id);
};

void InitClosure::operator() () {
	mbusConnection.connect(CALLBACK_MEMBER(this, &InitClosure::connected));
}

void InitClosure::connected() {
	mbusConnection.registerObject("keyboard",
			CALLBACK_MEMBER(this, &InitClosure::registered));
}

void InitClosure::registered(bragi_mbus::ObjectId object_id) {
	// do nothing for now
}

int main() {
	printf("Starting kbd\n");
	irq = helx::Irq::access(1);
	
	uintptr_t ports[] = { 0x60, 0x64 };
	HelHandle handle;
	HEL_CHECK(helAccessIo(ports, 2, &handle));
	HEL_CHECK(helEnableIo(handle));

	irq.wait(eventHub, CALLBACK_STATIC(nullptr, &onInterrupt));

	mbusConnection.setObjectHandler(&objectHandler);
	auto closure = new InitClosure();
	(*closure)();
	
	pid_t child = fork();
	assert(child != -1);
	if(!child) {
		execve("/usr/bin/vga_terminal", nullptr, nullptr);
	}
	
	while(true) {
		eventHub.defaultProcessEvents();
	}
}

