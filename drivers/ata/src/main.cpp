
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include <string>

#include <frigg/traits.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/glue-hel.hpp>
#include <helx.hpp>

#include "mbus.pb.h"

helx::EventHub eventHub = helx::EventHub::create();

int main() {
	printf("Starting ATA driver\n");

	const char *mbus_path = "config/mbus";
	HelHandle mbus_handle;
	HEL_CHECK(helRdOpen(mbus_path, strlen(mbus_path), &mbus_handle));
	helx::Client mbus_connect(mbus_handle);
	
	helx::Pipe mbus_pipe;
	HelError mbus_connect_error;
	mbus_connect.connectSync(eventHub, mbus_connect_error, mbus_pipe);
	HEL_CHECK(mbus_connect_error);

	managarm::mbus::CntRequest request;
	request.set_req_type(managarm::mbus::CntReqType::REGISTER);

	managarm::mbus::Capability *cap = request.add_caps();
	cap->set_name("block-device");

	std::string serialized;
	request.SerializeToString(&serialized);
	mbus_pipe.sendString(serialized.data(), serialized.size(), 1, 0);

	while(true)
		eventHub.defaultProcessEvents();
}
