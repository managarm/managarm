
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <unistd.h>
#include <fcntl.h>

#include <string>
#include <vector>
#include <experimental/optional>

#include <frigg/traits.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/callback.hpp>
#include <frigg/arch_x86/machine.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include <bragi/mbus.hpp>
#include <libcompose.hpp>
#include <libterminal.hpp>
#include <posix.pb.h>
#include <input.pb.h>

HelHandle ioHandle;

struct LibcAllocator {
	void *allocate(size_t length) {
		return malloc(length);
	}

	void free(void *pointer) {
		free(pointer);
	}
};

LibcAllocator allocator;

struct VgaDisplay : libterminal::Display {
	void setChar(int x, int y, char c, libterminal::Attribute attribute) override;
	void setCursor(int x, int y) override;
	void initializeScreen();

	uint8_t *videoMemoryPointer;
	int width = 80;
	int height = 25;
};

void VgaDisplay::setChar(int x, int y, char c, libterminal::Attribute attribute) {
	int color = 0x00;

	switch(attribute.fgColor) {
		case libterminal::kColorBlack: color += 0x00; break;
		case libterminal::kColorRed: color += 0x04; break;
		case libterminal::kColorGreen: color += 0x0A; break;
		case libterminal::kColorYellow: color += 0x0E; break;
		case libterminal::kColorBlue: color += 0x01; break;
		case libterminal::kColorMagenta: color += 0x0D; break;
		case libterminal::kColorCyan: color += 0x0B; break;
		case libterminal::kColorWhite: color += 0x0F; break;
		default: 
			printf("No valid fgColor!\n"); 
			abort();
	}

	switch(attribute.bgColor) {
		case libterminal::kColorBlack: color += 0x00; break;
		case libterminal::kColorRed: color += 0x40; break;
		case libterminal::kColorGreen: color += 0xA0; break;
		case libterminal::kColorYellow: color += 0xE0; break;
		case libterminal::kColorBlue: color += 0x10; break;
		case libterminal::kColorMagenta: color += 0xD0; break;
		case libterminal::kColorCyan: color += 0xB0; break;
		case libterminal::kColorWhite: color += 0xF0; break;
		default: 
			printf("No valid bgColor!\n"); 
			abort();
	}

	int position = y * width + x;
	videoMemoryPointer[position * 2] = c;
	videoMemoryPointer[position * 2 + 1] = color;	
}

void VgaDisplay::setCursor(int x, int y) {
	int position = x + width * y;

	uintptr_t ports[] = { 0x3D4, 0x3D5 };
	HEL_CHECK(helAccessIo(ports, 2, &ioHandle));
	HEL_CHECK(helEnableIo(ioHandle));

    frigg::arch_x86::ioOutByte(0x3D4, 0x0F);
    frigg::arch_x86::ioOutByte(0x3D5, position & 0xFF);
    frigg::arch_x86::ioOutByte(0x3D4, 0x0E);
    frigg::arch_x86::ioOutByte(0x3D5, (position >> 8) & 0xFF);
}

void VgaDisplay::initializeScreen() {
	// note: the vga test mode memory is actually 4000 bytes long
	HelHandle screen_memory;
	HEL_CHECK(helAccessPhysical(0xB8000, 0x1000, &screen_memory));

	// TODO: replace with drop-on-fork?
	void *actual_pointer;
	HEL_CHECK(helMapMemory(screen_memory, kHelNullHandle, nullptr,
			0, 0x1000, kHelMapReadWrite | kHelMapShareOnFork, &actual_pointer));
	videoMemoryPointer = (uint8_t *)actual_pointer;
	
	for(int y = 0; y < height; y++) {
		for(int x = 0; x < width; x++) {
			libterminal::Attribute attribute;
			attribute.fgColor = libterminal::kColorWhite;
			attribute.bgColor = libterminal::kColorBlack;
			setChar(x, y, ' ', attribute);
		}
	}
}

int masterFd;

void writeMaster(const char *string, size_t length) {
	write(masterFd, string, length);
}

void writeMaster(const char *string) {
	return writeMaster(string, strlen(string));
}

struct VgaComposeHandler : ComposeHandler {
	void input(std::string string) override;
};

void VgaComposeHandler::input(std::string string) {
	writeMaster(string.data(), string.length());
}

helx::EventHub eventHub = helx::EventHub::create();
bragi_mbus::Connection mbusConnection(eventHub);
VgaDisplay display;
libterminal::Emulator emulator(&display);

VgaComposeHandler vgaComposeHandle;
ComposeState composeState(&vgaComposeHandle);
Translator translator;


// --------------------------------------------------------
// RecvKbdClosure
// --------------------------------------------------------

struct RecvKbdClosure {
	RecvKbdClosure(helx::Pipe pipe);
	void operator() ();
	
private:
	void rcvdStringRequest(HelError error, int64_t msg_request,
		int64_t msg_seq, size_t length);
	char buffer[128];
	helx::Pipe pipe;	
};

RecvKbdClosure::RecvKbdClosure(helx::Pipe pipe)
: pipe(std::move(pipe)) { }

void RecvKbdClosure::operator() () {	
	HEL_CHECK(pipe.recvStringReq(buffer, 128, eventHub, 0, 0,
			CALLBACK_MEMBER(this, &RecvKbdClosure::rcvdStringRequest)));
}

void RecvKbdClosure::rcvdStringRequest(HelError error, int64_t msg_request,
		int64_t msg_seq, size_t length) {
	HEL_CHECK(error);

	managarm::input::ServerRequest request;
	request.ParseFromArray(buffer, length);

	if(request.request_type() == managarm::input::RequestType::CHANGE_STATE) {
		translator.changeState(request.code(), request.state());
	}else if(request.request_type() == managarm::input::RequestType::DOWN) {
		translator.keyDown(request.code());
		std::pair<KeyType, std::string> pair = translator.translate(request.code());
		
		composeState.keyPress(pair);
		
		//TODO: repair this
		if(pair.first == kKeySpecial && pair.second == "ArrowUp") {
			writeMaster("\e[A");
		}else if(pair.first == kKeySpecial && pair.second == "ArrowDown") {
			writeMaster("\e[B");
		}else if(pair.first == kKeySpecial && pair.second == "ArrowRight") {
			writeMaster("\e[C");
		}else if(pair.first == kKeySpecial && pair.second == "ArrowLeft") {
			writeMaster("\e[D");
		}else if(pair.first == kKeySpecial && pair.second == "Backspace") {
			writeMaster("\x08");
		}
	}else if(request.request_type() == managarm::input::RequestType::UP) {
		translator.keyUp(request.code());
	}

	(*this)();
}


// --------------------------------------------------------
// InitClosure
// --------------------------------------------------------

struct InitClosure {
	void operator() ();

private:
	void connected();
	void enumeratedKeyboards(std::vector<bragi_mbus::ObjectId> objects);
	void queriedKeyboards(HelHandle handle);
};

void InitClosure::operator() () {
	mbusConnection.connect(CALLBACK_MEMBER(this, &InitClosure::connected));
}

void InitClosure::connected() {
	mbusConnection.enumerate({ "keyboard" },
			CALLBACK_MEMBER(this, &InitClosure::enumeratedKeyboards));
}

void InitClosure::enumeratedKeyboards(std::vector<bragi_mbus::ObjectId> objects) {
	assert(objects.size() == 1);
	mbusConnection.queryIf(objects[0],
			CALLBACK_MEMBER(this, &InitClosure::queriedKeyboards));
}

void InitClosure::queriedKeyboards(HelHandle handle) {
	printf("queried keyboards\n");	
	
	auto closure = new RecvKbdClosure(helx::Pipe(handle));
	(*closure)();
}

struct ReadMasterClosure {
	void operator() ();

private:
	void connected(HelError error, HelHandle handle);
	void doRead();
	void recvdResponse(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);
	void recvdData(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);
	
	helx::Pipe pipe;
	char buffer[128];
	char data[128];
};

void ReadMasterClosure::operator() () {
	const char *posix_path = "local/posix";
	HelHandle posix_handle;
	HEL_CHECK(helRdOpen(posix_path, strlen(posix_path), &posix_handle));
	helx::Client posix_client(posix_handle);
	
	posix_client.connect(eventHub, CALLBACK_MEMBER(this, &ReadMasterClosure::connected));
}

void ReadMasterClosure::connected(HelError error, HelHandle handle) {
	HEL_CHECK(error);
	pipe = helx::Pipe(handle);
	doRead();
}

void ReadMasterClosure::doRead() {
	managarm::posix::ClientRequest request;
	request.set_request_type(managarm::posix::ClientRequestType::READ);
	request.set_fd(masterFd);
	request.set_size(128);

	std::string serialized;
	request.SerializeToString(&serialized);
	pipe.sendStringReq(serialized.data(), serialized.size(), 0, 0);
	
	HEL_CHECK(pipe.recvStringResp(buffer, 128, eventHub, 0, 0,
			CALLBACK_MEMBER(this, &ReadMasterClosure::recvdResponse)));
}

void ReadMasterClosure::recvdResponse(HelError error,
		int64_t msg_request, int64_t msg_seq, size_t length) {
	HEL_CHECK(error);
	
	managarm::posix::ServerResponse response;
	response.ParseFromArray(buffer, length);
	assert(response.error() == managarm::posix::Errors::SUCCESS);
	
	HEL_CHECK(pipe.recvStringResp(data, 128, eventHub, 0, 1,
			CALLBACK_MEMBER(this, &ReadMasterClosure::recvdData)));
}

void ReadMasterClosure::recvdData(HelError error,
		int64_t msg_request, int64_t msg_seq, size_t length) {
	HEL_CHECK(error);

	emulator.printString(std::string(data, length));

	doRead();
}

int main() {
	printf("Starting vga_terminal\n");

	display.initializeScreen();

	masterFd = open("/dev/pts/ptmx", O_RDWR);
	assert(masterFd != -1);

	auto closure = new InitClosure();
	(*closure)();

	int child = fork();
	assert(child != -1);
	if(!child) {
		int slave_fd = open("/dev/pts/1", O_RDWR);
		assert(slave_fd != -1);
		dup2(slave_fd, STDIN_FILENO);
		dup2(slave_fd, STDOUT_FILENO);
		dup2(slave_fd, STDERR_FILENO);

		execve("/usr/bin/bash", nullptr, nullptr);
	}

	auto read_master = new ReadMasterClosure();
	(*read_master)();
	
	while(true)
		eventHub.defaultProcessEvents();
}

