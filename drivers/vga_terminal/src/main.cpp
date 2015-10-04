
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <string>

#include <frigg/traits.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/async2.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include "char_device.pb.h"

uint8_t *videoMemoryPointer;
int xPosition = 0;
int yPosition = 0;
int width = 80;
int height = 25;


helx::EventHub eventHub = helx::EventHub::create();

struct LibcAllocator {
	void *allocate(size_t length) {
		return malloc(length);
	}

	void free(void *pointer) {
		free(pointer);
	}
};

LibcAllocator allocator;

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

void printChar(char character, uint8_t color) {
	if(character == '\n') {
		yPosition++;
		xPosition = 0;
	}else{
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
}
void printChar(char character) {
	printChar(character, 0x0F);
}

void printString(const char *array, size_t length){
	for(unsigned int i = 0; i < length; i++) {
		printChar(array[i]);
	}
}

void initializeScreen() {
	// note: the vga test mode memory is actually 4000 bytes long
	HelHandle screen_memory;
	HEL_CHECK(helAccessPhysical(0xB8000, 0x1000, &screen_memory));

	void *actual_pointer;
	HEL_CHECK(helMapMemory(screen_memory, kHelNullHandle, nullptr, 0x1000,
			kHelMapReadWrite, &actual_pointer));
	
	videoMemoryPointer = (uint8_t *)actual_pointer;
}

int main() {
	initializeScreen();

	for(int i = 1; i <= 30; i++) {
		char string[64];
		sprintf(string, "Hello %d\n", i);
		printString(string, strlen(string));
	}

	asm volatile ( "" : : : "memory" );

	while(true) {
		eventHub.defaultProcessEvents();
	}
}
