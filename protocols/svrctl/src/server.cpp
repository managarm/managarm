
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <iostream>

#include <bragi/helpers-std.hpp>
#include <core/logging.hpp>
#include <helix/ipc.hpp>

#include <protocols/svrctl/server.hpp>
#include "svrctl.bragi.hpp"

namespace protocols {
namespace svrctl {

static_assert(static_cast<int>(Error::success) == int(managarm::svrctl::Errors::SUCCESS));
static_assert(static_cast<int>(Error::deviceNotSupported)
		== int(managarm::svrctl::Errors::DEVICE_NOT_SUPPORTED));

struct ManagarmServerData {
	HelHandle controlLane;
};

async::result<void>
serveControl(const ControlOperations *ops) {
	ManagarmServerData sd;
	HEL_CHECK(helSyscall1(kHelCallSuper + 64, reinterpret_cast<HelWord>(&sd)));
	helix::UniqueLane lane{sd.controlLane};

	while(true) {
		auto [accept, recv_req] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::accept(
				helix_ng::recvInline())
		);
		if(accept.error() == kHelErrEndOfLane)
			co_return;
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();
		auto preamble = bragi::read_preamble(recv_req);
		if(preamble.id() == bragi::message_id<managarm::svrctl::DeviceBindRequest>) {
			assert(ops->bind);

			auto req = bragi::parse_head_only<managarm::svrctl::DeviceBindRequest>(recv_req);
			recv_req.reset();
			assert(req);

			auto error = co_await ops->bind(req->mbus_id());
			managarm::svrctl::DeviceBindResponse resp;
			resp.set_error(static_cast<managarm::svrctl::Errors>(error));

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
		}else{
			logPanic("serveControl: Unexpected request message ID {}", preamble.id());
		}
	}
}

} } // namespace protocols::svrctl
