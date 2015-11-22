
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
#include <posix.pb.h>
#include <input.pb.h>

uint8_t *videoMemoryPointer;
int xPosition = 0;
int yPosition = 0;
int width = 80;
int height = 25;
HelHandle ioHandle;

enum Status {
	kStatusNormal,
	kStatusEscape,
	kStatusCsi
};

struct LibcAllocator {
	void *allocate(size_t length) {
		return malloc(length);
	}

	void free(void *pointer) {
		free(pointer);
	}
};

LibcAllocator allocator;


void printString(std::string);

struct VgaComposeHandler : ComposeHandler {
	void input(std::string string) override;
};
void VgaComposeHandler::input(std::string string) {
	printString(string);
};

VgaComposeHandler vgaComposeHandle;
ComposeState composeState(&vgaComposeHandle);
Translator translator;

void setChar(char character, int x, int y, uint8_t color) {
	int position = y * width + x;
	videoMemoryPointer[position * 2] = character;
	videoMemoryPointer[position * 2 + 1] = color;
}
void setChar(char character, int x, int y) {
	setChar(character, x, y, 0x0F); 
}

char getChar(int x, int y) {
	int position = y * width + x;
	return videoMemoryPointer[position * 2];
}
uint8_t getColor(int x, int y) {
	int position = y * width + x;
	return videoMemoryPointer[position * 2 + 1];
}

Status status = kStatusNormal;
std::experimental::optional<int> currentNumber;
std::vector<int> params;
int currentTextColor = 0x0F;
int currentBackgroundColor = 0x00;

void setCursor(int x, int y);

void handleControlSeq(char character) {
	if(character == 'A') {
		int n = 1;
		if(!params.empty())
			n = params[0];
		if(n == 0)
			n = 1;

		if(yPosition - n >= 0) {
			yPosition -= n;
		}else{
			yPosition = 0;
		}
		setCursor(xPosition, yPosition);
	}else if(character == 'B') {
		int n = 1;
		if(!params.empty())
			n = params[0];
		if(n == 0)
			n = 1;
		
		if(yPosition + n <= height) {
			yPosition += n;
		}else{
			yPosition = height;
		}
		setCursor(xPosition, yPosition);
	}else if(character == 'C') {
		int n = 1;
		if(!params.empty())
			n = params[0];
		if(n == 0)
			n = 1;
		
		if(xPosition + n <= width) {
			xPosition += n;
		}else{
			xPosition = width;
		}
		setCursor(xPosition, yPosition);
	}else if(character == 'D') {
		int n = 1;
		if(!params.empty())
			n = params[0];
		if(n == 0)
			n = 1;
		
		if(xPosition - n >= 0) {
			xPosition -= n;
		}else{
			xPosition = 0;
		}
		setCursor(xPosition, yPosition);
	}else if(character == 'E') {
		int n = 1;
		if(!params.empty())
			n = params[0];
		
		if(yPosition + n <= height) {
			yPosition += n;
		}else{
			yPosition = height;
		}
		xPosition = 0;
		setCursor(xPosition, yPosition);
	}else if(character == 'F') {
		int n = 1;
		if(!params.empty())
			n = params[0];
		
		if(yPosition - n >= 0) {
			yPosition -= n;
		}else{
			yPosition = 0;
		}
		xPosition = 0;
		setCursor(xPosition, yPosition);
	}else if(character == 'G') {
		int n = 0;
		if(!params.empty())
			n = params[0];
		
		if(n >= 0 && n <= width){
			xPosition = n;
		}
		setCursor(xPosition, yPosition);
	}else if(character == 'J') {
		int n = 0;
		if(!params.empty())
			n = params[0];
		
		int color = currentTextColor | currentBackgroundColor;
		if(n == 0) {
			for(int i = xPosition; i <= width; i++) {
				setChar(' ', i, yPosition, color);
			}
			for(int i = yPosition + 1; i <= height; i++) {
				for(int j = 0; j < width; j++) {
					setChar(' ', i, j, color);
				}
			}
		}else if(n == 1) {
			for(int i = xPosition; i >= 0; i--) {
				setChar(' ', i, yPosition, color);
			}
			for(int i = yPosition - 1; i >= 0; i--) {
				for(int j = 0; j < width; j++) {
					setChar(' ', i, j, color);
				}
			}
		}else if(n == 2) {
			for(int i = 0; i <= height; i++) {
				for(int j = 0; j <= width; j++) {
					setChar(' ', i, j, color);
				}
			}
		}
	}else if(character == 'K') {
		int n = 0;
		if(!params.empty())
			n = params[0];

		int color = currentTextColor | currentBackgroundColor;
		if(n == 0) {
			for(int i = xPosition; i < width; i++) {
				setChar(' ', i, yPosition, color);
			}
		}else if(n == 1) {
			for(int i = xPosition; i >= 0; i--) {
				setChar(' ', i, yPosition, color);
			}
		}else if(n == 2) {
			for(int i = 0; i <= width; i++) {
				setChar(' ', i, yPosition, color);
			}
		}
	}else if(character == 'm') {
		if(!params.empty())
			params.push_back(0);

		for(size_t i = 0; i < params.size(); i++) {
			int n = params[i];
			switch(n) {
				case 30: 
					currentTextColor = 0x00;
					break;
				case 31:
					currentTextColor = 0x04;
					break;
				case 32:
					currentTextColor = 0x0A;
					break;
				case 33:
					currentTextColor = 0x0E;
					break;
				case 34:
					currentTextColor = 0x01;
					break;
				case 35:
					currentTextColor = 0x0D;
					break;
				case 36:
					currentTextColor = 0x0B;
					break;
				case 37:
					currentTextColor = 0x0F;
					break;
				case 40: 
					currentBackgroundColor = 0x00;
					break;
				case 41:
					currentBackgroundColor = 0x40;
					break;
				case 42:
					currentBackgroundColor = 0xA0;
					break;
				case 43:
					currentBackgroundColor = 0xE0;
					break;
				case 44:
					currentBackgroundColor = 0x10;
					break;
				case 45:
					currentBackgroundColor = 0xD0;
					break;
				case 46:
					currentBackgroundColor = 0xB0;
					break;
				case 47:
					currentBackgroundColor = 0xF0;
					break;
			}
		}
	}
}

void handleCsi(char character) {
	if(character >= '0' && character <= '9') {
		if(currentNumber) {
			currentNumber = *currentNumber * 10 + (character - '0');
		}else{
			currentNumber = character - '0';
		}
	}else if(character == ';') {
		if(currentNumber) {
			params.push_back(*currentNumber);
			currentNumber = std::experimental::nullopt;
		}else{
			params.push_back(0);
		}
	}else if(character >= 64 && character <= 126) {
		if(currentNumber) {
			params.push_back(*currentNumber);
			currentNumber = std::experimental::nullopt;
		}

		handleControlSeq(character);
		
		params.clear();
		status = kStatusNormal;
	}
}

void setCursor(int x, int y) {
	int position = x + width * y;

	uintptr_t ports[] = { 0x3D4, 0x3D5};
	HEL_CHECK(helAccessIo(ports, 2, &ioHandle));
	HEL_CHECK(helEnableIo(ioHandle));

    frigg::arch_x86::ioOutByte(0x3D4, 0x0F);
    frigg::arch_x86::ioOutByte(0x3D5, position & 0xFF);
    frigg::arch_x86::ioOutByte(0x3D4, 0x0E);
    frigg::arch_x86::ioOutByte(0x3D5, (position >> 8) & 0xFF);
}

void printChar(char character) {
	if(status == kStatusNormal) {
		if(character == 27) { //ASCII for escape
			status = kStatusEscape;
			return;
		}else if(character == '\n') {
			yPosition++;
			xPosition = 0;
		}else{
			int color = currentTextColor + currentBackgroundColor;
			setChar(character, xPosition, yPosition, color);
			xPosition++;
			if(xPosition >= width) {
				xPosition = 0;
				yPosition++;
			}
		}
		if(yPosition >= height) {
			for(int i = 1; i < height; i++) {
				for(int j = 0; j < width; j++) {
					char moved_char = getChar(j, i);
					uint8_t moved_color = getColor(j, i);
					setChar(moved_char, j, i - 1, moved_color);
				}
			}
			for(int j = 0; j < width; j++) {
				setChar(' ', j, height - 1, 0x0F);
			}
			yPosition = height - 1;
		}
		setCursor(xPosition, yPosition);
	}else if(status == kStatusEscape) {
		if(character == '[') {
			status = kStatusCsi;
		}
	}else if(status == kStatusCsi) {
		handleCsi(character);
	}
}

void printString(std::string string){
	for(unsigned int i = 0; i < string.size(); i++) {
		printChar(string[i]);
	}
}

void initializeScreen() {
	// note: the vga test mode memory is actually 4000 bytes long
	HelHandle screen_memory;
	HEL_CHECK(helAccessPhysical(0xB8000, 0x1000, &screen_memory));

	// TODO: replace with drop-on-fork?
	void *actual_pointer;
	HEL_CHECK(helMapMemory(screen_memory, kHelNullHandle, nullptr, 0x1000,
			kHelMapReadWrite | kHelMapShareOnFork, &actual_pointer));
	
	videoMemoryPointer = (uint8_t *)actual_pointer;
	
	int color = currentTextColor + currentBackgroundColor;
	for(int i = 0; i <= height; i++)
		for(int j = 0; j <= width; j++)
			setChar(' ', i, j, color);
}

helx::EventHub eventHub = helx::EventHub::create();
bragi_mbus::Connection mbusConnection(eventHub);


// --------------------------------------------------------
// RecvStrClosure
// --------------------------------------------------------

struct RecvStrClosure {
	RecvStrClosure(helx::Pipe pipe);
	void operator() ();
	
private:
	void rcvdStringRequest(HelError error, int64_t msg_request,
		int64_t msg_seq, size_t length);
	char buffer[128];
	helx::Pipe pipe;	
};

RecvStrClosure::RecvStrClosure(helx::Pipe pipe)
: pipe(std::move(pipe)) { }

void RecvStrClosure::operator() () {	
	HEL_CHECK(pipe.recvStringReq(buffer, 128, eventHub, 0, 0,
			CALLBACK_MEMBER(this, &RecvStrClosure::rcvdStringRequest)));
}

void RecvStrClosure::rcvdStringRequest(HelError error, int64_t msg_request,
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
		
		if(pair.first == kKeySpecial && pair.second == "ArrowUp") {
			printString("\e[A");
		}else if(pair.first == kKeySpecial && pair.second == "ArrowDown") {
			printString("\e[B");
		}else if(pair.first == kKeySpecial && pair.second == "ArrowRight") {
			printString("\e[C");
		}else if(pair.first == kKeySpecial && pair.second == "ArrowLeft") {
			printString("\e[D");
		}else if(pair.first == kKeySpecial && pair.second == "Backspace") {
			printString("\e[D");
			printString(" ");
			printString("\e[D");
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
	mbusConnection.enumerate("keyboard",
			CALLBACK_MEMBER(this, &InitClosure::enumeratedKeyboards));
}

void InitClosure::enumeratedKeyboards(std::vector<bragi_mbus::ObjectId> objects) {
	assert(objects.size() == 1);
	mbusConnection.queryIf(objects[0],
			CALLBACK_MEMBER(this, &InitClosure::queriedKeyboards));
}

void InitClosure::queriedKeyboards(HelHandle handle) {
	printf("queried keyboards\n");	
	
	auto closure = new RecvStrClosure(helx::Pipe(handle));
	(*closure)();
}

int masterFd;

struct ReadMasterClosure {
	void operator() ();

private:
	void connected(HelError error, HelHandle handle);
	void doRead();
	void recvdResponse(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);
	
	helx::Pipe pipe;
	char buffer[128];
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
	request.set_size(64);

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
	printString(response.buffer());

	doRead();
}

int main() {
	printf("Starting vga_terminal\n");
	initializeScreen();

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

		execve("zisa", nullptr, nullptr);
	}

	auto read_master = new ReadMasterClosure();
	(*read_master)();
	
	while(true)
		eventHub.defaultProcessEvents();
}

