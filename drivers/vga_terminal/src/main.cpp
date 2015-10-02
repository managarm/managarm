
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <string>

#include <frigg/traits.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/async2.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include "char_device.pb.h"

uint8_t *videoMemoryPointer;
uint8_t screenPosition;

helx::EventHub eventHub;

struct LibcAllocator {
	void *allocate(size_t length) {
		return malloc(length);
	}

	void free(void *pointer) {
		free(pointer);
	}
};

LibcAllocator allocator;

void printChar(char character, uint8_t color) {
	videoMemoryPointer[screenPosition] = character;
	screenPosition++;
	videoMemoryPointer[screenPosition] = color;
	screenPosition++;
}
void printChar(char character) {
	videoMemoryPointer[screenPosition] = character;
	screenPosition++;
	videoMemoryPointer[screenPosition] = 0x0F;
	screenPosition++;
}

void printString(const char *array, size_t length){
	for(unsigned int i = 0; i < length; i++) {
		printChar(array[i]);
	}
}

void screen() {
	// note: the vga test mode memory is actually 4000 bytes long
	HelHandle screen_memory;
	HEL_CHECK(helAccessPhysical(0xB8000, 0x1000, &screen_memory));

	void *actual_pointer;
	HEL_CHECK(helMapMemory(screen_memory, kHelNullHandle, nullptr, 0x1000,
			kHelMapReadWrite, &actual_pointer));
	
	videoMemoryPointer = (uint8_t *)actual_pointer;

	char string[] = "Hello World";
	printString(string, sizeof(string));

	asm volatile ( "" : : : "memory" );
}

void requestLoop(helx::Pipe pipe) {
	struct Context {
		Context(helx::Pipe pipe) : pipe(std::move(pipe)) { }

		uint8_t buffer[128];
		helx::Pipe pipe;
	};

	auto routine =
	frigg::asyncRepeatUntil(
		frigg::asyncSeq(
			frigg::wrapFuncPtr<helx::RecvStringFunction>([] (auto *context,
					void *cb_object, auto cb_function) {
				context->pipe.recvString(context->buffer, 128, eventHub,
						kHelAnyRequest, kHelAnySequence,
						cb_object, cb_function);
			}),
			frigg::wrapFunctor([] (auto *context, auto callback, HelError error,
					int64_t msg_request, int64_t msg_seq, size_t length) {
				HEL_CHECK(error);

				managarm::char_device::ClientRequest client_request;
				client_request.ParseFromArray(context->buffer, length);
				printString(client_request.chars().data(), client_request.chars().size());

				callback(true);
			})
		)
	);

	frigg::runAsync<Context>(allocator, routine, std::move(pipe));
}

void acceptLoop(helx::Server server) {
	struct Context {
		Context(helx::Server server) : server(std::move(server)) { }

		helx::Server server;
	};

	auto routine =
	frigg::asyncRepeatUntil(
		frigg::asyncSeq(
			frigg::wrapFuncPtr<helx::AcceptFunction>([] (auto *context,
					void *cb_object, auto cb_function) {
				context->server.accept(eventHub, cb_object, cb_function);
			}),
			frigg::wrapFunctor([] (Context *context, auto callback, HelError error,
					HelHandle handle) {
				HEL_CHECK(error);
				requestLoop(helx::Pipe(handle));
				callback(true);
			})
		)
	);
	
	frigg::runAsync<Context>(allocator, routine, std::move(server));
}

void on_connect(void *object, HelError error, HelHandle handle) {
	managarm::char_device::ClientRequest x;
	x.set_chars("Hello world");

	std::string serialized;
	x.SerializeToString(&serialized);

	helx::Pipe pipe(handle);
	pipe.sendString(serialized.data(), serialized.length(),
			0, 0);
}

int main() {
	screen();

	helx::Server server;
	helx::Client client;
	helx::Server::createServer(server, client);
	acceptLoop(std::move(server));

	client.connect(eventHub, nullptr, &on_connect);
	client.reset();

	while(true) {
		eventHub.defaultProcessEvents();
	}
}
