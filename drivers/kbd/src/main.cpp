
#include <algorithm>
#include <deque>
#include <iostream>

#include <stdio.h>
#include <string.h>
#include <linux/input.h>

#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <arch/io_space.hpp>
#include <async/result.hpp>
#include <boost/intrusive/list.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <libevbackend.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/mbus/client.hpp>

#include "spec.hpp"
#include "fs.pb.h"

arch::io_space base;
	
// --------------------------------------------
// Keyboard
// --------------------------------------------

std::shared_ptr<libevbackend::EventDevice> kbdEvntDev = std::make_shared<libevbackend::EventDevice>();
helix::UniqueIrq kbdIrq;

enum KeyboardStatus {
	kStatusNormal = 1,
	kStatusE0 = 2,
	kStatusE1First = 3,
	kStatusE1Second = 4
};

KeyboardStatus escapeStatus = kStatusNormal;
uint8_t e1Buffer;

int scanNormal(uint8_t data) {
	switch(data) {
		case 0x01: return KEY_ESC;
		case 0x02: return KEY_1;
		case 0x03: return KEY_2;
		case 0x04: return KEY_3;
		case 0x05: return KEY_4;
		case 0x06: return KEY_5;
		case 0x07: return KEY_6;
		case 0x08: return KEY_7;
		case 0x09: return KEY_8;
		case 0x0A: return KEY_9;
		case 0x0B: return KEY_0;
		case 0x0C: return KEY_MINUS;
		case 0x0D: return KEY_EQUAL;
		case 0x0E: return KEY_BACKSPACE;
		case 0x0F: return KEY_TAB;
		case 0x10: return KEY_Q;
		case 0x11: return KEY_W;
		case 0x12: return KEY_E;
		case 0x13: return KEY_R;
		case 0x14: return KEY_T;
		case 0x15: return KEY_Y;
		case 0x16: return KEY_U;
		case 0x17: return KEY_I;
		case 0x18: return KEY_O;
		case 0x19: return KEY_P;
		case 0x1A: return KEY_LEFTBRACE;
		case 0x1B: return KEY_RIGHTBRACE;
		case 0x1C: return KEY_ENTER;
		case 0x1D: return KEY_LEFTCTRL;
		case 0x1E: return KEY_A;
		case 0x1F: return KEY_S;
		case 0x20: return KEY_D;
		case 0x21: return KEY_F;
		case 0x22: return KEY_G;
		case 0x23: return KEY_H;
		case 0x24: return KEY_J;
		case 0x25: return KEY_K;
		case 0x26: return KEY_L;
		case 0x27: return KEY_SEMICOLON;
		case 0x28: return KEY_APOSTROPHE;
		case 0x29: return KEY_GRAVE;
		case 0x2A: return KEY_LEFTSHIFT;
		case 0x2B: return KEY_BACKSLASH;
		case 0x2C: return KEY_Z;
		case 0x2D: return KEY_X;
		case 0x2E: return KEY_C;
		case 0x2F: return KEY_V;
		case 0x30: return KEY_B;
		case 0x31: return KEY_N;
		case 0x32: return KEY_M;
		case 0x33: return KEY_COMMA;
		case 0x34: return KEY_DOT;
		case 0x35: return KEY_SLASH;
		case 0x36: return KEY_RIGHTSHIFT;
		case 0x37: return KEY_KPASTERISK;
		case 0x38: return KEY_LEFTALT;
		case 0x39: return KEY_SPACE;
		case 0x3A: return KEY_CAPSLOCK;
		case 0x3B: return KEY_F1;
		case 0x3C: return KEY_F2;
		case 0x3D: return KEY_F3;
		case 0x3E: return KEY_F4;
		case 0x3F: return KEY_F5;
		case 0x40: return KEY_F6;
		case 0x41: return KEY_F7;
		case 0x42: return KEY_F8;
		case 0x43: return KEY_F9;
		case 0x44: return KEY_F10;
		case 0x45: return KEY_NUMLOCK;
		case 0x46: return KEY_SCROLLLOCK;
		case 0x47: return KEY_KP7;
		case 0x48: return KEY_KP8;
		case 0x49: return KEY_KP9;
		case 0x4A: return KEY_KPMINUS;
		case 0x4B: return KEY_KP4;
		case 0x4C: return KEY_KP5;
		case 0x4D: return KEY_KP6;
		case 0x4E: return KEY_KPPLUS;
		case 0x4F: return KEY_KP1;
		case 0x50: return KEY_KP2;
		case 0x51: return KEY_KP3;
		case 0x52: return KEY_KP0;
		case 0x53: return KEY_KPDOT;
		case 0x57: return KEY_F11;
		case 0x58: return KEY_F12;
		default: return KEY_RESERVED;
	}
}

int scanE0(uint8_t data) {
	switch(data) {
		case 0x1C: return KEY_KPENTER;
		case 0x1D: return KEY_RIGHTCTRL;
		case 0x35: return KEY_KPSLASH;
		case 0x37: return KEY_SYSRQ;
		case 0x38: return KEY_RIGHTALT;
		case 0x47: return KEY_HOME;
		case 0x48: return KEY_UP;
		case 0x49: return KEY_PAGEUP;
		case 0x4B: return KEY_LEFT;
		case 0x4D: return KEY_RIGHT;
		case 0x4F: return KEY_END;
		case 0x50: return KEY_DOWN;
		case 0x51: return KEY_PAGEDOWN;
		case 0x52: return KEY_INSERT;
		case 0x53: return KEY_DELETE;
		case 0x5B: return KEY_LEFTMETA;
		case 0x5C: return KEY_RIGHTMETA;
		case 0x5D: return KEY_COMPOSE;
		default: return KEY_RESERVED;
	}
}

int scanE1(uint8_t data1, uint8_t data2) {
	if((data1 & 0x7F) == 0x1D && (data2 & 0X7F) == 0x45){
		return KEY_PAUSE;
	}else{
		return KEY_RESERVED;
	}
}

void handleKeyboardData(uint8_t data) {
	bool pressed = !(data & 0x80);
	if(escapeStatus == kStatusE1First) {
		e1Buffer = data;

		escapeStatus = kStatusE1Second;
	}else if(escapeStatus == kStatusE1Second) {
		assert((e1Buffer & 0x80) == (data & 0x80));
		kbdEvntDev->emitEvent(EV_KEY, scanNormal(data & 0x7F), pressed);
		escapeStatus = kStatusNormal;
	}else if(escapeStatus == kStatusE0) {
		kbdEvntDev->emitEvent(EV_KEY, scanNormal(data & 0x7F), pressed);
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
		
		kbdEvntDev->emitEvent(EV_KEY, scanNormal(data & 0x7F), pressed);
	}
}

// --------------------------------------------
// Mouse
// --------------------------------------------

std::shared_ptr<libevbackend::EventDevice> mouseEvntDev = std::make_shared<libevbackend::EventDevice>();
helix::UniqueIrq mouseIrq;

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
		
			mouseEvntDev->emitEvent(EV_REL, REL_X, movement_x);
			mouseEvntDev->emitEvent(EV_REL, REL_Y, movement_y);
			mouseByte = kMouseByte0;
		}
	}
}

// --------------------------------------------
// Functions
// --------------------------------------------

void sendByte(uint8_t data) {
	//base.store(kbd_register::command, write2ndNextByte);
	base.store(kbd_register::command, 0xD4);
	while(base.load(kbd_register::status) & status_bits::inBufferStatus) { };
	base.store(kbd_register::data, data);
}

void readDeviceData() {
	uint8_t status = base.load(kbd_register::command);
	if(!(status & 0x01))
		return;
	
	uint8_t data = base.load(kbd_register::data);
	if(status & 0x20) {
		handleMouseData(data);
	}else{
		handleKeyboardData(data);
	}
}

COFIBER_ROUTINE(cofiber::no_future, handleKbdIrqs(), ([=] {	
	while(true) {
		helix::AwaitIrq await_irq;
		auto &&submit = helix::submitAwaitIrq(kbdIrq, &await_irq, helix::Dispatcher::global());
		COFIBER_AWAIT submit.async_wait();
		HEL_CHECK(await_irq.error());
		
		readDeviceData();
	}
}))

COFIBER_ROUTINE(cofiber::no_future, handleMouseIrqs(), ([=] {	
	while(true) {
		helix::AwaitIrq await_irq;
		auto &&submit = helix::submitAwaitIrq(mouseIrq, &await_irq, helix::Dispatcher::global());
		COFIBER_AWAIT submit.async_wait();
		HEL_CHECK(await_irq.error());
		
		readDeviceData();
	}
}))

COFIBER_ROUTINE(cofiber::no_future, runKbd(), ([=] {
	// Create an mbus object for the partition.
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();
	
	std::unordered_map<std::string, std::string> descriptor {
		{ "unix.devtype", "block" },
		{ "unix.devname", "event0" },
	};
	auto object = COFIBER_AWAIT root.createObject("kbd", descriptor,
			[=] (mbus::AnyQuery query) -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		libevbackend::serveDevice(kbdEvntDev, std::move(local_lane));

		async::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.async_get();
	});
}))

COFIBER_ROUTINE(cofiber::no_future, runMouse(), ([=] {
	// Create an mbus object for the partition.
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();
	
	std::unordered_map<std::string, std::string> descriptor {
		{ "unix.devtype", "block" },
		{ "unix.devname", "event1" },
	};
	auto object = COFIBER_AWAIT root.createObject("mouse", descriptor,
			[=] (mbus::AnyQuery query) -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		libevbackend::serveDevice(mouseEvntDev, std::move(local_lane));

		async::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.async_get();
	});
}))

int main() {
	printf("Starting ps/2\n");
	
	HelHandle kbd_handle;
	HEL_CHECK(helAccessIrq(1, &kbd_handle));
	kbdIrq = helix::UniqueIrq(kbd_handle);
	
	HelHandle mouse_handle;
	HEL_CHECK(helAccessIrq(12, &mouse_handle));
	mouseIrq = helix::UniqueIrq(mouse_handle);
	
	uintptr_t ports[] = { DATA, STATUS };
	HelHandle handle;
	HEL_CHECK(helAccessIo(ports, 2, &handle));
	HEL_CHECK(helEnableIo(handle));

	base = arch::global_io.subspace(DATA);

	// disable both devices
	base.store(kbd_register::command, disable1stPort);
	base.store(kbd_register::command, disable2ndPort);

	// flush the output buffer
	while(base.load(kbd_register::status) & status_bits::outBufferStatus)
		base.load(kbd_register::data);

	// enable interrupt for second device
	base.store(kbd_register::command, readByte0);
	uint8_t configuration = base.load(kbd_register::data);
	configuration |= 0x02;
	base.store(kbd_register::command, writeByte0);
	base.store(kbd_register::data, configuration);

	// enable both devices
	base.store(kbd_register::command, enable1stPort);
	base.store(kbd_register::command, enable2ndPort);

	// enables mouse response
	sendByte(0xF4);
	mouseState = kMouseWaitForAck;

	runKbd();
	runMouse();

	handleKbdIrqs();
	handleMouseIrqs();
	
	while(true) {
		helix::Dispatcher::global().dispatch();
	}

	return 0;
}

