
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include <string>

#include <frigg/traits.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/callback.hpp>
#include <helx.hpp>

#include "mbus.pb.h"

struct Allocator {
	void *allocate(size_t length) {
		return malloc(length);
	}

	void free(void *pointer) {
		free(pointer);
	}
};

Allocator allocator;

helx::EventHub eventHub = helx::EventHub::create();
helx::Pipe mbusPipe;

// --------------------------------------------------------
// MbusClosure
// --------------------------------------------------------

struct MbusClosure : public frigg::BaseClosure<MbusClosure> {
	void operator() ();

private:
	void recvdRequest(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length);

	uint8_t buffer[128];
};

void MbusClosure::operator() () {
	auto callback = CALLBACK_MEMBER(this, &MbusClosure::recvdRequest);
	HEL_CHECK(mbusPipe.recvString(buffer, 128, eventHub,
			kHelAnyRequest, 0, callback.getObject(), callback.getFunction()));
}

void MbusClosure::recvdRequest(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	managarm::mbus::SvrRequest svr_request;
	svr_request.ParseFromArray(buffer, length);

	switch(svr_request.req_type()) {
	case managarm::mbus::SvrReqType::BROADCAST:
		// TODO: only receive certain broadcasts
		break;
	case managarm::mbus::SvrReqType::REQUIRE_IF: {
		helx::Pipe server_side, client_side;
		helx::Pipe::createFullPipe(server_side, client_side);
		mbusPipe.sendDescriptor(client_side.getHandle(), msg_request, 1);
		client_side.reset();
	} break;
	default:
		assert(!"Illegal request type");
	}

	(*this)();
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting ATA driver\n");

	const char *mbus_path = "config/mbus";
	HelHandle mbus_handle;
	HEL_CHECK(helRdOpen(mbus_path, strlen(mbus_path), &mbus_handle));
	helx::Client mbus_connect(mbus_handle);
	
	HelError mbus_connect_error;
	mbus_connect.connectSync(eventHub, mbus_connect_error, mbusPipe);
	HEL_CHECK(mbus_connect_error);

	frigg::runClosure<MbusClosure>(allocator);

	managarm::mbus::CntRequest request;
	request.set_req_type(managarm::mbus::CntReqType::REGISTER);

	managarm::mbus::Capability *cap = request.add_caps();
	cap->set_name("block-device");

	std::string serialized;
	request.SerializeToString(&serialized);
	mbusPipe.sendString(serialized.data(), serialized.size(), 1, 0);

	while(true)
		eventHub.defaultProcessEvents();
}

