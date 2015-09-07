
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <unordered_map>

#include <string>

#include <frigg/traits.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/callback.hpp>
#include <frigg/async.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include "fs.pb.h"

namespace util = frigg::util;
namespace async = frigg::async;

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


struct OpenFile {
	void *imagePtr;
	size_t fileSize;
};

int64_t nextFd = 1;
std::unordered_map<int64_t, OpenFile> allOpenFiles;

void openFile(managarm::fs::ClientRequest request, helx::Pipe pipe, int64_t msg_request) {
	OpenFile open_file;

	HelHandle image_handle;
	std::string path = "initrd/" + request.filename();

	HEL_CHECK(helRdOpen(path.data(), path.size(), &image_handle));
	HEL_CHECK(helMemoryInfo(image_handle, &open_file.fileSize));
	printf("openfile_size: %u\n", open_file.fileSize);
	HEL_CHECK(helMapMemory(image_handle, kHelNullHandle, nullptr, open_file.fileSize,
			kHelMapReadOnly, &open_file.imagePtr));

	int64_t fd = nextFd;
	nextFd++;
	allOpenFiles[fd] = open_file;

	managarm::fs::ServerResponse response;
	response.set_fd(fd);

	std::string serialized;
	response.SerializeToString(&serialized);

	pipe.sendString(serialized.data(), serialized.length(), msg_request, 0);
	printf("opened file\n");
}

void readFile(managarm::fs::ClientRequest request, helx::Pipe pipe, int64_t msg_request) {
	managarm::fs::ServerResponse response;

	if(allOpenFiles.find(request.fd()) == allOpenFiles.end()) {
		response.set_error(-1);
	}else{
		OpenFile &open_file = allOpenFiles[request.fd()];

		response.set_buffer((char *)open_file.imagePtr, request.size());
		printf("read file\n");
	}

	std::string serialized;
	response.SerializeToString(&serialized);
	pipe.sendString(serialized.data(), serialized.length(), msg_request, 0);	
}

void writeFile(managarm::fs::ClientRequest request, helx::Pipe pipe, int64_t msg_request) {
	managarm::fs::ServerResponse response;
	response.set_error(-1);

	std::string serialized;
	response.SerializeToString(&serialized);

	pipe.sendString(serialized.data(), serialized.length(), msg_request, 0);
}

void closeFile(managarm::fs::ClientRequest request, helx::Pipe pipe, int64_t msg_request) {
	managarm::fs::ServerResponse response;

	if(allOpenFiles.find(request.fd()) == allOpenFiles.end()) {
		response.set_error(-1);
	}else{
		allOpenFiles.erase(request.fd());	
		printf("closed file\n");
	}

	std::string serialized;
	response.SerializeToString(&serialized);
	pipe.sendString(serialized.data(), serialized.length(), msg_request, 0);	
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

			managarm::fs::ClientRequest client_request;
			client_request.ParseFromArray(context.buffer, length);

			if(client_request.request_type() == managarm::fs::ClientRequest::OPEN) {
				openFile(client_request, context.pipe, msg_request);
			}
			else if(client_request.request_type() == managarm::fs::ClientRequest::READ) {
				readFile(client_request, context.pipe, msg_request);
			}
			else if(client_request.request_type() == managarm::fs::ClientRequest::WRITE) {
				writeFile(client_request, context.pipe, msg_request);
			}
			else if(client_request.request_type() == managarm::fs::ClientRequest::CLOSE) {
				closeFile(client_request, context.pipe, msg_request);
			}

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
 			HEL_CHECK(error);

			auto on_complete = [] (ProcessContext &context) { };
			helx::Pipe pipe(handle);
			async::run(allocator, processRequest, ProcessContext(pipe), on_complete);

			callback();
		})
	)
);

int main() {
	helx::Server server;
	helx::Client client;
	helx::Server::createServer(server, client);

	auto on_complete = [] (AcceptContext &context) { };
	async::run(allocator, processAccept, AcceptContext(server), on_complete);

	const char *parent_path = "local/parent";
	HelHandle parent_handle;
	HEL_CHECK(helRdOpen(parent_path, strlen(parent_path), &parent_handle));
	HEL_CHECK(helSendDescriptor(parent_handle, client.getHandle(), 0, 0));

	while(true) {
		eventHub.defaultProcessEvents();
	}
}
