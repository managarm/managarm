
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <iostream>

#include <bragi/helpers-std.hpp>
#include <core/dispatch.hpp>
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

namespace {

struct HandleBind {
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::svrctl::DeviceBindRequest &&req,
	           helix::BorrowedDescriptor conversation, bragi::preamble) {
		assert(ops->bind);

		auto error = co_await ops->bind(req.mbus_id());
		managarm::svrctl::DeviceBindResponse resp;
		resp.set_error(static_cast<managarm::svrctl::Errors>(error));

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
		co_return {};
	}

	const ControlOperations *ops;
};

} // namespace anonymous

async::result<void>
serveControl(const ControlOperations *ops) {
	ManagarmServerData sd;
	HEL_CHECK(helSyscall1(kHelCallSuper + 64, reinterpret_cast<HelWord>(&sd)));
	helix::UniqueLane lane{sd.controlLane};

	while(true) {
		auto res = co_await dispatchRequest<
			managarm::svrctl::DeviceBindRequest
		>(lane, HandleBind{ops});
		if(!res) {
			if(res.error() == DispatchError::shutdown)
				co_return;
			std::cout << "svrctl: dispatch error" << std::endl;
			continue;
		}
	}
}

} } // namespace protocols::svrctl
