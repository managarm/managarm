
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
#include <helix/ipc.hpp>
#include <libevbackend.hpp>
#include <protocols/mbus/client.hpp>
#include <async/queue.hpp>
#include <helix/timer.hpp>

#include "spec.hpp"
#include "ps2.hpp"

namespace {
	constexpr bool logPackets = false;
	constexpr bool logMouse = false;
}
constexpr int default_timeout = 100'000'000;

// --------------------------------------------------------------------
// Controller
// --------------------------------------------------------------------

Controller::Controller() {
	HEL_CHECK(helAccessIrq(1, &_irq1Handle));
	_irq1 = helix::UniqueIrq(_irq1Handle);

	HEL_CHECK(helAccessIrq(12, &_irq12Handle));
	_irq12 = helix::UniqueIrq(_irq12Handle);

	uintptr_t ports[] = { DATA, STATUS };
	HelHandle handle;
	HEL_CHECK(helAccessIo(ports, 2, &handle));
	HEL_CHECK(helEnableIo(handle));

	_space = arch::global_io.subspace(DATA);
}

async::detached Controller::init() {
	_space = arch::global_io.subspace(DATA);

	// disable both devices
	submitCommand(controller_cmd::DisablePort{}, 0);
	submitCommand(controller_cmd::DisablePort{}, 1);

	// flush the output buffer
	while(_space.load(kbd_register::status) & status_bits::outBufferStatus)
		_space.load(kbd_register::data);

	// enable interrupt for second device
	auto configuration = submitCommand(controller_cmd::GetByte0{});
	_hasSecondPort = configuration & (1 << 5);

	configuration |= 0b11; // enable interrupts
	configuration &= ~(1 << 6); // disable translation

	submitCommand(controller_cmd::SetByte0{}, configuration);

	// enable devices
	submitCommand(controller_cmd::EnablePort{}, 0);
	if(_hasSecondPort)
		submitCommand(controller_cmd::EnablePort{}, 1);

	// From this point on, data read from the data port belongs to the device.
	_portsOwnData = true;
	handleIrqsFor(_irq1, 0);
	handleIrqsFor(_irq12, 1);
	// Firmware might have left ports enabled, and the user might have typed during boot.
	// Reset the IRQ status to ensure that the following code works.
	HEL_CHECK(helAcknowledgeIrq(_irq1.getHandle(), kHelAckKick, 0));
	HEL_CHECK(helAcknowledgeIrq(_irq12.getHandle(), kHelAckKick, 0));

	// Initialize devices.
	printf("ps2-hid: Setting up first port\n");
	_ports[0] = new Port{this, 0};
	co_await _ports[0]->init();

	if (_ports[0]->isDead())
		printf("ps2-hid: No device on first port\n");

	if (_hasSecondPort) {
		printf("ps2-hid: Setting up second port\n");
		_ports[1] = new Port{this, 1};
		co_await _ports[1]->init();

		if (_ports[1]->isDead())
			printf("ps2-hid: No device on second port\n");
	}

	printf("ps2-hid: Initialization done\n");
}

void Controller::sendCommandByte(uint8_t byte) {
	bool inEmpty = helix::busyWaitUntil(default_timeout, [&] {
		return !(_space.load(kbd_register::status) & status_bits::inBufferStatus);
	});
	if(!inEmpty)
		printf("ps2-hid: Controller failed to empty input buffer\n");
	// There is not a load that we can do if the controller misbehaves; for now we just abort.
	assert(inEmpty);

	_space.store(kbd_register::command, byte);
}

void Controller::sendDataByte(uint8_t byte) {
	bool inEmpty = helix::busyWaitUntil(default_timeout, [&] {
		return !(_space.load(kbd_register::status) & status_bits::inBufferStatus);
	});
	if(!inEmpty)
		printf("ps2-hid: Controller failed to empty input buffer\n");
	// There is not a load that we can do if the controller misbehaves; for now we just abort.
	assert(inEmpty);

	_space.store(kbd_register::data, byte);
}

std::optional<uint8_t> Controller::recvResponseByte(uint64_t timeout) {
	assert(!_portsOwnData);

	if (timeout) {
		uint64_t start, end, current;

		HEL_CHECK(helGetClock(&start));
		end = start + timeout;
		current = start;

		while (!(_space.load(kbd_register::status) 
				& status_bits::outBufferStatus) && current < end)
			HEL_CHECK(helGetClock(&current));

		bool cancelled = current >= end;

		if (cancelled)
			return std::nullopt;

		return _space.load(kbd_register::data);
	}

	while (!(_space.load(kbd_register::status) & status_bits::outBufferStatus))
		;
	return _space.load(kbd_register::data);
}

void Controller::submitCommand(controller_cmd::DisablePort, int port) {
	if (port == 0)
		sendCommandByte(disable1stPort);
	else if (port == 1)
		sendCommandByte(disable2ndPort);
}

void Controller::submitCommand(controller_cmd::EnablePort, int port) {
	if (port == 0)
		sendCommandByte(enable1stPort);
	else if (port == 1)
		sendCommandByte(enable2ndPort);
}

uint8_t Controller::submitCommand(controller_cmd::GetByte0) {
	sendCommandByte(readByte0);

	auto result = recvResponseByte(default_timeout);

	assert(result != std::nullopt && "timed out");

	return result.value();
}

void Controller::submitCommand(controller_cmd::SetByte0, uint8_t val) {
	sendCommandByte(writeByte0);
	sendDataByte(val);
}

void Controller::submitCommand(controller_cmd::SendBytePort2) {
	sendCommandByte(0xD4); // TODO: define a constant?
}

async::detached Controller::handleIrqsFor(helix::UniqueIrq &irq, int port) {
	assert(_portsOwnData);

	uint64_t sequence = 0;
	while(true) {
		auto await = co_await helix_ng::awaitEvent(irq, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		// TODO: detect whether we want to ack/nack
		processData(port);
		HEL_CHECK(helAcknowledgeIrq(irq.getHandle(), kHelAckAcknowledge, sequence));
	}
}

bool Controller::processData(int port) {
	size_t count = 0;
	while (_space.load(kbd_register::status) & status_bits::outBufferStatus) {
		auto val = _space.load(kbd_register::data);

		if (logPackets)
			printf("ps2-hid: received byte 0x%02x on port %d!\n", val, port);

		if (_ports[port]->isDead()) {
			printf("ps2-hid: received irq for non-existent device!\n");
		} else {
			_ports[port]->pushByte(val);
		}

		count++;
	}

	return count > 0;
}

// --------------------------------------------------------------------
// Controller::Port
// --------------------------------------------------------------------

Controller::Port::Port(Controller *controller, int port)
: _controller{controller}, _port{port}, _deviceType{} {
}

async::result<void> Controller::Port::init() {
	auto res1 = co_await submitCommand(device_cmd::DisableScan{});
	if (!res1) {
		_dead = true;
		co_return;
	}

	auto res2 = co_await submitCommand(device_cmd::Identify{});
	if (!res2) {
		_dead = true;
		co_return;
	}
	_deviceType = res2.value();

	if (_deviceType.keyboard)
		_device = std::make_unique<KbdDevice>(this);
	if (_deviceType.mouse)
		_device = std::make_unique<MouseDevice>(this);

	if (!_device) {
		_dead = true;
		co_return;
	}
	co_await _device->run();
}

async::result<void> Controller::KbdDevice::run() {
	//set scancode 1
	auto res1 = co_await submitCommand(device_cmd::SetScancodeSet{}, 1);
	assert(res1);

	// make sure it's used
	auto res2 = co_await submitCommand(device_cmd::GetScancodeSet{});
	assert(res2);
	assert(res2.value() == 1);

	//setup evdev stuff
	_evDev = std::make_shared<libevbackend::EventDevice>();

	_evDev->enableEvent(EV_KEY, KEY_A);
	_evDev->enableEvent(EV_KEY, KEY_B);
	_evDev->enableEvent(EV_KEY, KEY_C);
	_evDev->enableEvent(EV_KEY, KEY_D);
	_evDev->enableEvent(EV_KEY, KEY_E);
	_evDev->enableEvent(EV_KEY, KEY_F);
	_evDev->enableEvent(EV_KEY, KEY_G);
	_evDev->enableEvent(EV_KEY, KEY_H);
	_evDev->enableEvent(EV_KEY, KEY_I);
	_evDev->enableEvent(EV_KEY, KEY_J);
	_evDev->enableEvent(EV_KEY, KEY_K);
	_evDev->enableEvent(EV_KEY, KEY_L);
	_evDev->enableEvent(EV_KEY, KEY_M);
	_evDev->enableEvent(EV_KEY, KEY_N);
	_evDev->enableEvent(EV_KEY, KEY_O);
	_evDev->enableEvent(EV_KEY, KEY_P);
	_evDev->enableEvent(EV_KEY, KEY_Q);
	_evDev->enableEvent(EV_KEY, KEY_R);
	_evDev->enableEvent(EV_KEY, KEY_S);
	_evDev->enableEvent(EV_KEY, KEY_T);
	_evDev->enableEvent(EV_KEY, KEY_U);
	_evDev->enableEvent(EV_KEY, KEY_V);
	_evDev->enableEvent(EV_KEY, KEY_W);
	_evDev->enableEvent(EV_KEY, KEY_X);
	_evDev->enableEvent(EV_KEY, KEY_Y);
	_evDev->enableEvent(EV_KEY, KEY_Z);
	_evDev->enableEvent(EV_KEY, KEY_1);
	_evDev->enableEvent(EV_KEY, KEY_2);
	_evDev->enableEvent(EV_KEY, KEY_3);
	_evDev->enableEvent(EV_KEY, KEY_4);
	_evDev->enableEvent(EV_KEY, KEY_5);
	_evDev->enableEvent(EV_KEY, KEY_6);
	_evDev->enableEvent(EV_KEY, KEY_7);
	_evDev->enableEvent(EV_KEY, KEY_8);
	_evDev->enableEvent(EV_KEY, KEY_9);
	_evDev->enableEvent(EV_KEY, KEY_0);
	_evDev->enableEvent(EV_KEY, KEY_ENTER);
	_evDev->enableEvent(EV_KEY, KEY_ESC);
	_evDev->enableEvent(EV_KEY, KEY_BACKSPACE);
	_evDev->enableEvent(EV_KEY, KEY_TAB);
	_evDev->enableEvent(EV_KEY, KEY_SPACE);
	_evDev->enableEvent(EV_KEY, KEY_MINUS);
	_evDev->enableEvent(EV_KEY, KEY_EQUAL);
	_evDev->enableEvent(EV_KEY, KEY_LEFTBRACE);
	_evDev->enableEvent(EV_KEY, KEY_RIGHTBRACE);
	_evDev->enableEvent(EV_KEY, KEY_BACKSLASH);
	_evDev->enableEvent(EV_KEY, KEY_SEMICOLON);
	_evDev->enableEvent(EV_KEY, KEY_COMMA);
	_evDev->enableEvent(EV_KEY, KEY_DOT);
	_evDev->enableEvent(EV_KEY, KEY_SLASH);
	_evDev->enableEvent(EV_KEY, KEY_HOME);
	_evDev->enableEvent(EV_KEY, KEY_PAGEUP);
	_evDev->enableEvent(EV_KEY, KEY_DELETE);
	_evDev->enableEvent(EV_KEY, KEY_END);
	_evDev->enableEvent(EV_KEY, KEY_PAGEDOWN);
	_evDev->enableEvent(EV_KEY, KEY_RIGHT);
	_evDev->enableEvent(EV_KEY, KEY_LEFT);
	_evDev->enableEvent(EV_KEY, KEY_DOWN);
	_evDev->enableEvent(EV_KEY, KEY_UP);
	_evDev->enableEvent(EV_KEY, KEY_LEFTCTRL);
	_evDev->enableEvent(EV_KEY, KEY_LEFTSHIFT);
	_evDev->enableEvent(EV_KEY, KEY_LEFTALT);
	_evDev->enableEvent(EV_KEY, KEY_LEFTMETA);
	_evDev->enableEvent(EV_KEY, KEY_RIGHTCTRL);
	_evDev->enableEvent(EV_KEY, KEY_RIGHTSHIFT);
	_evDev->enableEvent(EV_KEY, KEY_RIGHTALT);
	_evDev->enableEvent(EV_KEY, KEY_RIGHTMETA);
	_evDev->enableEvent(EV_KEY, KEY_F1);
	_evDev->enableEvent(EV_KEY, KEY_F2);
	_evDev->enableEvent(EV_KEY, KEY_F3);
	_evDev->enableEvent(EV_KEY, KEY_F4);
	_evDev->enableEvent(EV_KEY, KEY_F5);
	_evDev->enableEvent(EV_KEY, KEY_F6);
	_evDev->enableEvent(EV_KEY, KEY_F7);
	_evDev->enableEvent(EV_KEY, KEY_F8);
	_evDev->enableEvent(EV_KEY, KEY_F9);
	_evDev->enableEvent(EV_KEY, KEY_F10);
	_evDev->enableEvent(EV_KEY, KEY_F11);
	_evDev->enableEvent(EV_KEY, KEY_F12);
	_evDev->enableEvent(EV_KEY, KEY_KP1);
	_evDev->enableEvent(EV_KEY, KEY_KP2);
	_evDev->enableEvent(EV_KEY, KEY_KP3);
	_evDev->enableEvent(EV_KEY, KEY_KP4);
	_evDev->enableEvent(EV_KEY, KEY_KP5);
	_evDev->enableEvent(EV_KEY, KEY_KP6);
	_evDev->enableEvent(EV_KEY, KEY_KP7);
	_evDev->enableEvent(EV_KEY, KEY_KP8);
	_evDev->enableEvent(EV_KEY, KEY_KP9);
	_evDev->enableEvent(EV_KEY, KEY_KP0);
	_evDev->enableEvent(EV_KEY, KEY_KPMINUS);
	_evDev->enableEvent(EV_KEY, KEY_KPPLUS);
	_evDev->enableEvent(EV_KEY, KEY_KPDOT);
	_evDev->enableEvent(EV_KEY, KEY_KPASTERISK);
	_evDev->enableEvent(EV_KEY, KEY_KPSLASH);
	_evDev->enableEvent(EV_KEY, KEY_KPENTER);

	// Create an mbus object for the partition.
	auto root = co_await mbus::Instance::global().getRoot();

	mbus::Properties descriptor{
		{"unix.subsystem", mbus::StringItem{"input"}}
	};

	auto handler = mbus::ObjectHandler{}
	.withBind([=] () -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		libevbackend::serveDevice(_evDev, std::move(local_lane));

		async::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.async_get();
	});

	co_await root.createObject("ps2kbd", descriptor, std::move(handler));

	// Finalize the device initialization.
	auto res3 = co_await _port->submitCommand(device_cmd::EnableScan{});
	assert(res3);

	processReports();
}

async::result<void> Controller::MouseDevice::run() {
	_deviceType = _port->deviceType();

	// attempt to enable scroll wheel
	auto res1 = co_await submitCommand(device_cmd::SetReportRate{}, 200);
	assert(res1);

	res1 = co_await submitCommand(device_cmd::SetReportRate{}, 100);
	assert(res1);

	res1 = co_await submitCommand(device_cmd::SetReportRate{}, 80);
	assert(res1);

	auto res2 = co_await _port->submitCommand(device_cmd::Identify{});
	assert(res2);

	auto type = res2.value();
	assert(type.mouse); // ensure the mouse is still a mouse
	_deviceType.hasScrollWheel = _deviceType.hasScrollWheel || type.hasScrollWheel;

	// attempt to enable the 4th and 5th buttons
	res1 = co_await submitCommand(device_cmd::SetReportRate{}, 200);
	assert(res1);

	res1 = co_await submitCommand(device_cmd::SetReportRate{}, 200);
	assert(res1);

	res1 = co_await submitCommand(device_cmd::SetReportRate{}, 80);
	assert(res1);

	res2 = co_await _port->submitCommand(device_cmd::Identify{});
	assert(res2);

	type = res2.value();
	assert(type.mouse); // ensure the mouse is still a mouse
	_deviceType.has5Buttons = _deviceType.has5Buttons || type.has5Buttons;

	// set report rate to the default
	res1 = co_await submitCommand(device_cmd::SetReportRate{}, 100);
	assert(res1);

	// setup evdev stuff
	_evDev = std::make_shared<libevbackend::EventDevice>();

	_evDev->enableEvent(EV_REL, REL_X);
	_evDev->enableEvent(EV_REL, REL_Y);

	if (_deviceType.hasScrollWheel)
		_evDev->enableEvent(EV_REL, REL_WHEEL);

	_evDev->enableEvent(EV_KEY, BTN_LEFT);
	_evDev->enableEvent(EV_KEY, BTN_RIGHT);
	_evDev->enableEvent(EV_KEY, BTN_MIDDLE);

	if (_deviceType.has5Buttons) {
		_evDev->enableEvent(EV_KEY, BTN_SIDE);
		_evDev->enableEvent(EV_KEY, BTN_EXTRA);
	}

	// Create an mbus object for the partition.
	auto root = co_await mbus::Instance::global().getRoot();

	mbus::Properties descriptor{
		{"unix.subsystem", mbus::StringItem{"input"}}
	};

	auto handler = mbus::ObjectHandler{}
	.withBind([=] () -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		libevbackend::serveDevice(_evDev, std::move(local_lane));

		async::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.async_get();
	});

	co_await root.createObject("ps2mouse", descriptor, std::move(handler));

	// Finalize the device initialization.
	auto res3 = co_await _port->submitCommand(device_cmd::EnableScan{});
	assert(res3);

	processReports();
}

async::detached Controller::MouseDevice::processReports() {
	while (true) {
		uint8_t byte0, byte1, byte2, byte3 = 0;
		byte0 = (co_await _port->pullByte()).value();
		byte1 = (co_await _port->pullByte()).value();
		byte2 = (co_await _port->pullByte()).value();

		if (_deviceType.has5Buttons || _deviceType.hasScrollWheel)
			byte3 = (co_await _port->pullByte()).value();

		int movement_x = (int)byte1 - (int)((byte0 << 4) & 0x100);
		int movement_y = (int)byte2 - (int)((byte0 << 3) & 0x100);

		int movement_wheel = 0;
		if (_deviceType.hasScrollWheel) {
			movement_wheel = (int)(byte3 & 0x7) - (int)(byte3 & 0x8);
		}

		if (!(byte0 & 8)) {
			printf("ps2-hid: desync? first byte is %02x\n", byte0);
			continue;
		}


		if (byte0 & 0xC0) {
			printf("ps2-hid: overflow\n");
			continue;
		}

		if(logMouse) {
			printf("ps2-hid: mouse packet dump:\n");
			printf("ps2-hid: x move: %d, y move: %d, z move: %d\n",
					movement_x, movement_y, movement_wheel);
			printf("ps2-hid: left: %d, right: %d, middle: %d\n",
					(byte0 & 1) > 0, (byte0 & 2) > 0, (byte0 & 4));
			printf("ps2-hid: 4th: %d, 5th: %d\n",
					(byte3 & 4) > 0, (byte3 & 5) > 0);
		}

		_evDev->emitEvent(EV_REL, REL_X, byte1 ? movement_x : 0);
		_evDev->emitEvent(EV_REL, REL_Y, byte2 ? -movement_y : 0);

		if (_deviceType.hasScrollWheel) {
			_evDev->emitEvent(EV_REL, REL_WHEEL, -movement_wheel);
		}

		_evDev->emitEvent(EV_KEY, BTN_LEFT, byte0 & 1);
		_evDev->emitEvent(EV_KEY, BTN_RIGHT, byte0 & 2);
		_evDev->emitEvent(EV_KEY, BTN_MIDDLE, byte0 & 4);

		if (_deviceType.has5Buttons) {
			_evDev->emitEvent(EV_KEY, BTN_SIDE, byte3 & 4);
			_evDev->emitEvent(EV_KEY, BTN_EXTRA, byte3 & 5);
		}

		_evDev->emitEvent(EV_SYN, SYN_REPORT, 0);
		_evDev->notify();
	}
}

int scanNormal(uint8_t data) {
	switch (data) {
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
	switch (data) {
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
	if ((data1 & 0x7F) == 0x1D && (data2 & 0x7F) == 0x45) {
		return KEY_PAUSE;
	} else {
		return KEY_RESERVED;
	}
}

async::detached Controller::KbdDevice::processReports() {
	while (true) {
		int key = -1;
		bool pressed = false;
		uint8_t byte0, byte1, byte2;

		byte0 = (co_await _port->pullByte()).value();

		if (byte0 == 0xE0) {
			byte1 = (co_await _port->pullByte()).value();
			key = scanE0(byte1 & 0x7F);
			pressed = !(byte1 & 0x80);
		} else if (byte0 == 0xE1) {
			byte1 = (co_await _port->pullByte()).value();
			byte2 = (co_await _port->pullByte()).value();
			key = scanE1(byte1, byte2);
			pressed = !(byte1 & 0x80);
			assert((byte1 & 0x80) == (byte2 & 0x80));
		} else {
			key = scanNormal(byte0 & 0x7F);
			pressed = !(byte0 & 0x80);
		}

		_evDev->emitEvent(EV_KEY, key, pressed);
		_evDev->emitEvent(EV_SYN, SYN_REPORT, 0);
		_evDev->notify();
	}
}

void Controller::Port::pushByte(uint8_t byte) {
	_dataQueue.put(byte);
}

async::result<std::optional<uint8_t>>
Controller::Port::pullByte(async::cancellation_token ct) {
	auto result = co_await _dataQueue.async_get(ct);

	// We need to convert the frg::optional to a std::optional here.
	if(!result)
		co_return std::nullopt;
	co_return *result;
}

static DeviceType determineTypeById(uint16_t id) {
	if (id == 0)
		return DeviceType{.mouse = true};
	if (id == 0x3)
		return DeviceType{.mouse = true, .hasScrollWheel = true};
	if (id == 0x4)
		return DeviceType{.mouse = true, .has5Buttons = true};
	if (id == 0xAB41 || id == 0xABC1 || id == 0xAB83)
		return DeviceType{.keyboard = true};

	printf("ps2-hid: unknown device id %04x, please submit a bug report\n", id);
	return DeviceType{}; // we assume nothing
}

async::result<frg::expected<Ps2Error, DeviceType>>
Controller::Port::submitCommand(device_cmd::Identify tag) {
	auto cmdResp = co_await transferByte(0xF2);
	if (!cmdResp)
		co_return Ps2Error::timeout;
	if (*cmdResp != 0xFA) {
		printf("ps2-hid: Expected ACK after Identify command on port %d, got 0x%02x\n",
				_port, *cmdResp);
		co_return Ps2Error::nack;
	}

	auto data0 = co_await recvResponseByte(default_timeout);
	auto data1 = co_await recvResponseByte(default_timeout);

	if (!data0 && !data1) {
		// Ancient AT keyboard (identify command returns nothing).
		co_return DeviceType{.keyboard = true};
	} else if (!data1) {
		co_return determineTypeById(static_cast<uint16_t>(*data0));
	} else {
		co_return determineTypeById((static_cast<uint16_t>(*data0) << 8)
				| static_cast<uint16_t>(*data1));
	}
}

async::result<frg::expected<Ps2Error>>
Controller::Port::submitCommand(device_cmd::DisableScan tag) {
	auto cmdResp = co_await transferByte(0xF5);
	if (!cmdResp)
		co_return Ps2Error::timeout;
	if (*cmdResp != 0xFA) {
		printf("ps2-hid: Expected ACK after DisableScan command on port %d, got 0x%02x\n",
				_port, *cmdResp);
		co_return Ps2Error::nack;
	}

	co_return {};
}

async::result<frg::expected<Ps2Error>>
Controller::Port::submitCommand(device_cmd::EnableScan tag) {
	auto cmdResp = co_await transferByte(0xF4);
	if (!cmdResp)
		co_return Ps2Error::timeout;
	if (*cmdResp != 0xFA) {
		printf("ps2-hid: Expected ACK after EnableScan command on port %d, got 0x%02x\n",
				_port, *cmdResp);
		co_return Ps2Error::nack;
	}

	co_return {};
}

async::result<frg::expected<Ps2Error>>
Controller::MouseDevice::submitCommand(device_cmd::SetReportRate tag, int rate) {
	auto cmdResp = co_await _port->transferByte(0xF3);
	if (!cmdResp)
		co_return Ps2Error::timeout;
	if (*cmdResp != 0xFA) {
		printf("ps2-hid: Expected ACK after SetReportRate command on port %d, got 0x%02x\n",
				_port->getIndex(), *cmdResp);
		co_return Ps2Error::nack;
	}

	auto outResp = co_await _port->transferByte(rate);
	if (!outResp)
		co_return Ps2Error::timeout;
	if (*outResp != 0xFA) {
		printf("ps2-hid: Expected ACK after SetReportRate output byte on port %d, got 0x%02x\n",
				_port->getIndex(), *outResp);
		co_return Ps2Error::nack;
	}

	co_return {};
}

async::result<frg::expected<Ps2Error>>
Controller::KbdDevice::submitCommand(device_cmd::SetScancodeSet tag, int set) {
	// If set == 0, this would be a GetScancodeSet command.
	assert(set);

	auto cmdResp = co_await _port->transferByte(0xF0);
	if (!cmdResp)
		co_return Ps2Error::timeout;
	if (*cmdResp != 0xFA) {
		printf("ps2-hid: Expected ACK after SetScancodeSet data byte on port %d, got 0x%02x\n",
				_port->getIndex(), *cmdResp);
		co_return Ps2Error::nack;
	}

	auto outResp = co_await _port->transferByte(set);
	if (!outResp)
		co_return Ps2Error::timeout;
	if (*outResp != 0xFA) {
		printf("ps2-hid: Expected ACK after setScancodeSet output byte on port %d, got 0x%02x\n",
				_port->getIndex(), *outResp);
		co_return Ps2Error::nack;
	}

	co_return {};
}

async::result<frg::expected<Ps2Error, int>>
Controller::KbdDevice::submitCommand(device_cmd::GetScancodeSet tag) {
	auto cmdResp = co_await _port->transferByte(0xF0);
	if (!cmdResp)
		co_return Ps2Error::timeout;
	if (*cmdResp != 0xFA) {
		printf("ps2-hid: Expected ACK after SetScancodeSet data byte on port %d, got 0x%02x\n",
				_port->getIndex(), *cmdResp);
		co_return Ps2Error::nack;
	}

	auto outResp = co_await _port->transferByte(0);
	if (!outResp)
		co_return Ps2Error::timeout;
	if (*outResp != 0xFA) {
		printf("ps2-hid: Expected ACK after setScancodeSet output byte on port %d, got 0x%02x\n",
				_port->getIndex(), *outResp);
		co_return Ps2Error::nack;
	}

	auto setResp = co_await _port->recvResponseByte(default_timeout);
	if (!setResp)
		co_return Ps2Error::timeout;
	co_return setResp.value();
}

void Controller::Port::sendByte(uint8_t byte) {
	if (_port == 1) {
		_controller->submitCommand(controller_cmd::SendBytePort2{});
	}

	_controller->sendDataByte(byte);
}


async::result<std::optional<uint8_t>> Controller::Port::transferByte(uint8_t byte) {
	while (true) {
		sendByte(byte);

		auto resp = co_await recvResponseByte(default_timeout);

		// 0xFE requests a retransmission.
		if (!resp || *resp != 0xFE)
			co_return resp;
	}
}

async::result<std::optional<uint8_t>> Controller::Port::recvResponseByte(uint64_t timeout) {
	frg::optional<uint8_t> result;
	if (timeout) {
		async::cancellation_event ev;
		helix::TimeoutCancellation timer{timeout, ev};

		result = co_await _dataQueue.async_get(ev);
		co_await timer.retire();
	} else {
		result = co_await _dataQueue.async_get();
	}

	// We need to convert the frg::optional to a std::optional here.
	if(!result)
		co_return std::nullopt;
	co_return *result;
}

Controller *_controller;

int main() {
	std::cout << "ps2-hid: Starting driver" << std::endl;

	_controller = new Controller;

	{
		async::queue_scope scope{helix::globalQueue()};

		_controller->init();
	}

	async::run_forever(helix::globalQueue()->run_token(), helix::currentDispatcher);

	return 0;
}
