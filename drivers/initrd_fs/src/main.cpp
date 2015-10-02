
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
#include <frigg/async2.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include "fs.pb.h"

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

	managarm::fs::ServerResponse response;

	HelError open_error = helRdOpen(path.data(), path.size(), &image_handle);	
	if(open_error == kHelErrNone) {
		HEL_CHECK(helMemoryInfo(image_handle, &open_file.fileSize));
		printf("openfile_size: %u\n", open_file.fileSize);
		HEL_CHECK(helMapMemory(image_handle, kHelNullHandle, nullptr, open_file.fileSize,
				kHelMapReadOnly, &open_file.imagePtr));

		int64_t fd = nextFd;
		nextFd++;
		allOpenFiles[fd] = open_file;
		
		response.set_error(managarm::fs::Errors::SUCCESS);
		response.set_fd(fd);

		printf("opened file\n");	
	}else if(open_error == kHelErrNoSuchPath) {
		response.set_error(managarm::fs::Errors::FILE_NOT_FOUND);
	}else{
		HEL_CHECK(open_error);
	}

	std::string serialized;
	response.SerializeToString(&serialized);
	pipe.sendString(serialized.data(), serialized.length(), msg_request, 0);	
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

				managarm::fs::ClientRequest client_request;
				client_request.ParseFromArray(context->buffer, length);

				if(client_request.request_type() == managarm::fs::ClientRequest::OPEN) {
//FIXME:					openFile(client_request, std::move(context.pipe), msg_request);
				}else if(client_request.request_type() == managarm::fs::ClientRequest::READ) {
//FIXME:					readFile(client_request, std::move(context.pipe), msg_request);
				}else if(client_request.request_type() == managarm::fs::ClientRequest::WRITE) {
//FIXME:					writeFile(client_request, std::move(context.pipe), msg_request);
				}else if(client_request.request_type() == managarm::fs::ClientRequest::CLOSE) {
//FIXME:					closeFile(client_request, std::move(context.pipe), msg_request);
				}

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

int main() {
	helx::Server server;
	helx::Client client;
	helx::Server::createServer(server, client);
	acceptLoop(std::move(server));
	
	const char *parent_path = "local/parent";
	HelHandle parent_handle;
	HEL_CHECK(helRdOpen(parent_path, strlen(parent_path), &parent_handle));
	HEL_CHECK(helSendDescriptor(parent_handle, client.getHandle(), 0, 0));
	client.reset();

	while(true) {
		eventHub.defaultProcessEvents();
	}
}
