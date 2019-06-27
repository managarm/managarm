
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <iostream>

#include <helix/ipc.hpp>
#include <helix/await.hpp>

#include <protocols/svrctl/server.hpp>
#include "svrctl.pb.h"

namespace protocols {
namespace svrctl {

static_assert(static_cast<int>(Error::success) == managarm::svrctl::Error::SUCCESS);
static_assert(static_cast<int>(Error::deviceNotSupported)
		== managarm::svrctl::Error::DEVICE_NOT_SUPPORTED);

struct ManagarmServerData {
	HelHandle controlLane;
};

async::result<void>
serveControl(const ControlOperations *ops) {
	ManagarmServerData sd;
	HEL_CHECK(helSyscall1(kHelCallSuper + 64, reinterpret_cast<HelWord>(&sd)));
	helix::UniqueLane lane{sd.controlLane};

	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&initiate = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		co_await initiate.async_wait();

		if(accept.error() == kHelErrEndOfLane)
			co_return;
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();

		managarm::svrctl::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.req_type() == managarm::svrctl::CntReqType::CTL_BIND) {
			helix::SendBuffer send_resp;

			assert(ops->bind);
			auto error = co_await ops->bind(req.mbus_id());

			managarm::svrctl::SvrResponse resp;
			resp.set_error(static_cast<managarm::svrctl::Error>(error));

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else{
			helix::SendBuffer send_resp;

			managarm::svrctl::SvrResponse resp;
			resp.set_error(managarm::svrctl::Error::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}
	}
}

} } // namespace protocols::svrctl
