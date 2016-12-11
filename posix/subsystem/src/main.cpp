
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <iostream>

#include <cofiber.hpp>

#include "common.hpp"
#include "device.hpp"
#include "vfs.hpp"
#include "process.hpp"
#include "exec.hpp"
//FIXME #include "dev_fs.hpp"
//FIXME #include "pts_fs.hpp"
//FIXME #include "sysfile_fs.hpp"
//FIXME #include "extern_fs.hpp"
#include <posix.pb.h>

bool traceRequests = false;

//FIXME: helx::EventHub eventHub = helx::EventHub::create();
//FIXME: helx::Client mbusConnect;
//FIXME: helx::Pipe ldServerPipe;
//FIXME: helx::Pipe mbusPipe;

COFIBER_ROUTINE(cofiber::no_future, serve(SharedProcess self,
		helix::UniqueDescriptor p), ([self, lane = std::move(p)] {
	using M = helix::AwaitMechanism;

	while(true) {
		helix::Accept<M> accept;
		helix::RecvBuffer<M> recv_req;

		char buffer[256];
		helix::submitAsync(lane, {
			helix::action(&accept, kHelItemAncillary),
			helix::action(&recv_req, buffer, 256)
		}, helix::Dispatcher::global());
		COFIBER_AWAIT accept.future();
		COFIBER_AWAIT recv_req.future();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::posix::CntRequest req;
		req.ParseFromArray(buffer, recv_req.actualLength());
		if(req.request_type() == managarm::posix::CntReqType::OPEN) {
			auto file = COFIBER_AWAIT open(req.path());
			int fd = self.attachFile(file);
			(void)fd;

			helix::SendBuffer<M> send_resp;
			helix::PushDescriptor<M> push_passthrough;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&push_passthrough, getPassthroughLane(file))
			}, helix::Dispatcher::global());
			
			COFIBER_AWAIT send_resp.future();
			COFIBER_AWAIT push_passthrough.future();
			
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_passthrough.error());
		}else{
			std::cout << "posix: Illegal request" << std::endl;
			helix::SendBuffer<M> send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size())
			}, helix::Dispatcher::global());
			COFIBER_AWAIT send_resp.future();
			HEL_CHECK(send_resp.error());
		}
	}
}))

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	std::cout << "Starting posix-subsystem" << std::endl;
	
	execute(SharedProcess::createInit(), "posix-init");

	while(true)
		helix::Dispatcher::global().dispatch();
}

