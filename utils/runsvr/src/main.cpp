
#include <iostream>

#include <async/jump.hpp>
#include <helix/memory.hpp>
#include <protocols/mbus/client.hpp>
#include <svrctl.pb.h>

// ----------------------------------------------------------------------------
// svrctl handling.
// ----------------------------------------------------------------------------

helix::UniqueLane svrctlLane;
async::jump foundSvrctl;

COFIBER_ROUTINE(async::result<void>, enumerateSvrctl(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("class", "svrctl")
	});
	
	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) {
//		std::cout << "runsvr: Found svrctl" << std::endl;

		svrctlLane = helix::UniqueLane(COFIBER_AWAIT entity.bind());
		foundSvrctl.trigger();
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
	COFIBER_AWAIT foundSvrctl.async_wait();
	COFIBER_RETURN();
}))

COFIBER_ROUTINE(async::result<void>, runServer(const char *name), ([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvInline recv_resp;

	managarm::svrctl::CntRequest req;
	req.set_req_type(managarm::svrctl::CntReqType::SVR_RUN);
	req.set_name(name);

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(svrctlLane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::svrctl::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::svrctl::Error::SUCCESS);
	
	COFIBER_RETURN();
}))

// ----------------------------------------------------------------
// Freestanding mbus functions.
// ----------------------------------------------------------------

COFIBER_ROUTINE(cofiber::no_future, enumerateAndRunServer(const char *name), ([=] {
	COFIBER_AWAIT enumerateSvrctl();

	COFIBER_AWAIT runServer(name);
}))

int main(int argc, char **argv) {
	std::cout << "svrctl: Running " << argv[1] << std::endl;

	{
		async::queue_scope scope{helix::globalQueue()};
		enumerateAndRunServer(argv[1]);
	}

	helix::globalQueue()->run();
	
	return 0;
}


