
#include <stdio.h>
#include <stdlib.h>

#include <libchain/all.hpp>

#include <fs.pb.h>
#include <libnet.hpp>

#include "udp.hpp"
#include "tcp.hpp"
#include "dns.hpp"
#include "arp.hpp"
#include "usernet.hpp"
#include "ethernet.hpp"
#include "network.hpp"

namespace libnet {

Network::Network(NetDevice &devide)
: device(device) { }

// --------------------------------------------------------
// Client
// --------------------------------------------------------

Client::Client(helx::EventHub &event_hub, Network &net)
: eventHub(event_hub), net(net), objectHandler(*this), mbusConnection(eventHub) {
	mbusConnection.setObjectHandler(&objectHandler);
}

void Client::init(frigg::CallbackPtr<void()> callback) {
	auto closure = new InitClosure(*this, callback);
	(*closure)();
}

// --------------------------------------------------------
// Client::ObjectHandler
// --------------------------------------------------------

Client::ObjectHandler::ObjectHandler(Client &client)
: client(client) { }

void Client::ObjectHandler::requireIf(bragi_mbus::ObjectId object_id,
		frigg::CallbackPtr<void(HelHandle)> callback) {
	helx::Pipe local, remote;
	helx::Pipe::createFullPipe(local, remote);
	callback(remote.getHandle());
	remote.reset();

	auto closure = new Connection(client.eventHub, client.net, std::move(local));
	(*closure)();
}

// --------------------------------------------------------
// Client::InitClosure
// --------------------------------------------------------

Client::InitClosure::InitClosure(Client &client, frigg::CallbackPtr<void()> callback)
: client(client), callback(callback) { }

void Client::InitClosure::operator() () {
	client.mbusConnection.connect(CALLBACK_MEMBER(this, &InitClosure::connected));
}

void Client::InitClosure::connected() {
	client.mbusConnection.registerObject("network",
			CALLBACK_MEMBER(this, &InitClosure::registered));
}

void Client::InitClosure::registered(bragi_mbus::ObjectId object_id) {
	callback();
}

// --------------------------------------------------------
// Connection
// --------------------------------------------------------

Connection::Connection(helx::EventHub &event_hub, Network &net, helx::Pipe pipe)
: eventHub(event_hub), net(net), pipe(std::move(pipe)), nextHandle(1) { }

void Connection::operator() () {
	HEL_CHECK(pipe.recvStringReq(buffer, 128, eventHub, kHelAnyRequest, 0,
			CALLBACK_MEMBER(this, &Connection::recvRequest)));
}

Network &Connection::getNet() {
	return net;
}

int Connection::attachOpenFile(OpenFile *file) {
	int handle = nextHandle++;
	fileHandles.insert(std::make_pair(handle, file));
	return handle;
}

OpenFile *Connection::getOpenFile(int handle) {
	return fileHandles.at(handle);
}

void Connection::recvRequest(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	HEL_CHECK(error);

	managarm::fs::CntRequest request;
	request.ParseFromArray(buffer, length);
	
	if(request.req_type() == managarm::fs::CntReqType::OPEN) {
		auto action = libchain::apply([this, request, msg_request] () {
			managarm::fs::SvrResponse response;

			if(request.path() == "ip+udp") {
				int handle = attachOpenFile(new OpenFile);
				response.set_error(managarm::fs::Errors::SUCCESS);
				response.set_file_type(managarm::fs::FileType::SOCKET);
				response.set_fd(handle);
			}else{
				response.set_error(managarm::fs::Errors::FILE_NOT_FOUND);
			}
			
			std::string serialized;
			response.SerializeToString(&serialized);
			pipe.sendStringResp(serialized.data(), serialized.size(),
					msg_request, 0);	
		}, libchain::nullary);
	
		libchain::run(action);
	}else if(request.req_type() == managarm::fs::CntReqType::CONNECT) {
		auto action = libchain::apply([this, request, msg_request] () {
			managarm::fs::SvrResponse response;
			
			auto file = getOpenFile(request.fd());
			file->address = Ip4Address(8, 8, 8, 8);
			response.set_error(managarm::fs::Errors::SUCCESS);

			std::string serialized;
			response.SerializeToString(&serialized);
			pipe.sendStringResp(serialized.data(), serialized.size(),
					msg_request, 0);
		}, libchain::nullary);

		libchain::run(action);
	}/*else if(request.req_type() == managarm::fs::CntReqType::READ) {
		auto closure = new ReadClosure(*this, msg_request, std::move(request));
		(*closure)();
	}*/else{
		fprintf(stderr, "Illegal request type\n");
		abort();
	}

	(*this)();
}

} // namespace libnet

