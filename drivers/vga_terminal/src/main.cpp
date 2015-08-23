
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <string>

#include <frigg/traits.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/callback.hpp>
#include <frigg/async.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include "char_device.pb.h"

namespace util = frigg::util;
namespace async = frigg::async;

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

struct ProcessContext {
	ProcessContext(helx::Pipe pipe) : pipe(pipe) { }

	uint8_t buffer[128];
	helx::Pipe pipe;
};

auto processRequest = async::repeatWhile(
	async::lambda([] (ProcessContext &context, util::Callback<void(bool)> callback) {
		callback(true);
	}),
	async::seq(
		async::lambda([] (ProcessContext &context, util::Callback<void(HelError, int64_t,
				int64_t, size_t)> callback) {
			context.pipe.recvString(context.buffer, 128, eventHub,
					kHelAnyRequest, kHelAnySequence,
					callback.getObject(), callback.getFunction());
		}),
		async::lambda([] (ProcessContext &context, util::Callback<void()> callback, 
				HelError error, int64_t msg_request, int64_t msg_seq, size_t length) {
			HEL_CHECK(error);

			managarm::char_device::ClientRequest client_request;
			client_request.ParseFromArray(context.buffer, length);
			printString(client_request.chars().data(), client_request.chars().size());

			callback();
		})
	)
);

struct AcceptContext {
	AcceptContext(helx::Server server) : server(server) { }

	helx::Server server;
};

auto processAccept = async::repeatWhile(
	async::lambda([] (AcceptContext &context, util::Callback<void(bool)> callback) {
		callback(true);
	}),
	async::seq(
		async::lambda([] (AcceptContext &context, util::Callback<void(HelError, HelHandle)> callback) {
			context.server.accept(eventHub, callback.getObject(), callback.getFunction());
		}),
 		async::lambda([] (AcceptContext &context, util::Callback<void()> callback,
 				HelError error, HelHandle handle) {
// 			HEL_CHECK(error);

			auto on_complete = [] (ProcessContext &context) { };
			helx::Pipe pipe(handle);
			async::run(allocator, processRequest, ProcessContext(pipe), on_complete);

			callback();
 		})
	)
);

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

	auto on_complete = [] (AcceptContext &context) { };
	async::run(allocator, processAccept, AcceptContext(server), on_complete);

	client.connect(eventHub, nullptr, &on_connect);

	while(true) {
		eventHub.defaultProcessEvents();
	}
}
