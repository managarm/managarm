
#include <iostream>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <helix/ipc.hpp>

#include "svrctl.bragi.hpp"
#include <protocols/svrctl/server.hpp>

namespace protocols {
namespace svrctl {

static_assert(static_cast<int>(Error::success) == int(managarm::svrctl::Error::SUCCESS));
static_assert(
    static_cast<int>(Error::deviceNotSupported) ==
    int(managarm::svrctl::Error::DEVICE_NOT_SUPPORTED)
);

struct ManagarmServerData {
	HelHandle controlLane;
};

async::result<void> serveControl(const ControlOperations *ops) {
	ManagarmServerData sd;
	HEL_CHECK(helSyscall1(kHelCallSuper + 64, reinterpret_cast<HelWord>(&sd)));
	helix::UniqueLane lane{sd.controlLane};

	while (true) {
		auto [accept, recv_req] =
		    co_await helix_ng::exchangeMsgs(lane, helix_ng::accept(helix_ng::recvInline()));
		if (accept.error() == kHelErrEndOfLane)
			co_return;
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();

		managarm::svrctl::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		recv_req.reset();
		if (req.req_type() == managarm::svrctl::CntReqType::CTL_BIND) {
			assert(ops->bind);
			auto error = co_await ops->bind(req.mbus_id());

			managarm::svrctl::SvrResponse resp;
			resp.set_error(static_cast<managarm::svrctl::Error>(error));

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		} else {
			managarm::svrctl::SvrResponse resp;
			resp.set_error(managarm::svrctl::Error::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
			    conversation, helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}
	}
}

} // namespace svrctl
} // namespace protocols
