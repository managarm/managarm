
#include <fcntl.h>
#include <sys/stat.h>
#include <iostream>

#include <async/jump.hpp>
#include <helix/memory.hpp>
#include <protocols/mbus/client.hpp>
#include <kernlet.pb.h>
#include "common.hpp"

// ----------------------------------------------------------------------------
// kernletctl handling.
// ----------------------------------------------------------------------------

helix::UniqueLane kernletCtlLane;
async::jump foundKernletCtl;

COFIBER_ROUTINE(async::result<void>, enumerateCtl(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("class", "kernletctl")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) {
		std::cout << "kernletcc: Found kernletctl" << std::endl;

		kernletCtlLane = helix::UniqueLane(COFIBER_AWAIT entity.bind());
		foundKernletCtl.trigger();
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
	COFIBER_AWAIT foundKernletCtl.async_wait();
	COFIBER_RETURN();
}))

COFIBER_ROUTINE(async::result<helix::UniqueDescriptor>, upload(const void *elf, size_t size,
		std::vector<BindType> bind_types), ([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::SendBuffer send_data;
	helix::RecvInline recv_resp;
	helix::PullDescriptor pull_kernlet;

	managarm::kernlet::CntRequest req;
	req.set_req_type(managarm::kernlet::CntReqType::UPLOAD);

	for(auto bt : bind_types) {
		managarm::kernlet::ParameterType proto;
		switch(bt) {
		case BindType::offset: proto = managarm::kernlet::OFFSET; break;
		case BindType::memoryView: proto = managarm::kernlet::MEMORY_VIEW; break;
		default:
			assert(!"Unexpected binding type");
		}
		req.add_bind_types(proto);
	}

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(kernletCtlLane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&send_data, elf, size, kHelItemChain),
			helix::action(&recv_resp, kHelItemChain),
			helix::action(&pull_kernlet));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_kernlet.error());

	managarm::kernlet::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::kernlet::Error::SUCCESS);
	std::cout << "kernletcc: Upload success" << std::endl;

	COFIBER_RETURN(std::move(pull_kernlet.descriptor()));
}))

// ----------------------------------------------------------------------------
// kernletcc mbus interface.
// ----------------------------------------------------------------------------

COFIBER_ROUTINE(cofiber::no_future, serveCompiler(helix::UniqueLane lane),
		([lane = std::move(lane)] () {
	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;
		helix::RecvInline recv_code;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req, kHelItemChain),
				helix::action(&recv_code));
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		HEL_CHECK(recv_code.error());

		auto conversation = accept.descriptor();

		managarm::kernlet::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.req_type() == managarm::kernlet::CntReqType::COMPILE) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor push_kernlet;

			std::vector<BindType> bind_types;
			for(int i = 0; i < req.bind_types_size(); i++) {
				auto proto = req.bind_types(i);
				BindType bt;
				switch(proto) {
				case managarm::kernlet::OFFSET: bt = BindType::offset; break;
				case managarm::kernlet::MEMORY_VIEW: bt = BindType::memoryView; break;
				default:
					assert(!"Unexpected binding type");
				}
				bind_types.push_back(bt);
			}

			auto elf = compileFafnir(reinterpret_cast<const uint8_t *>(recv_code.data()),
					recv_code.length(), bind_types);
			auto object = COFIBER_AWAIT upload(elf.data(), elf.size(), bind_types);

			managarm::kernlet::SvrResponse resp;
			resp.set_error(managarm::kernlet::Error::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&push_kernlet, object));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else{
			throw std::runtime_error("Unexpected request type");
		}
	}
}))

COFIBER_ROUTINE(async::result<void>, createCompilerObject(), ([] {
	// Create an mbus object for the device.
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	mbus::Properties descriptor{
		{"class", mbus::StringItem{"kernletcc"}},
	};

	auto handler = mbus::ObjectHandler{}
	.withBind([=] () -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		serveCompiler(std::move(local_lane));

		async::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.async_get();
	});

	COFIBER_AWAIT root.createObject("kernletcc", descriptor, std::move(handler));
}))

// ----------------------------------------------------------------
// Freestanding mbus functions.
// ----------------------------------------------------------------

COFIBER_ROUTINE(cofiber::no_future, asyncMain(const char **args), ([=] {
	COFIBER_AWAIT enumerateCtl();
	COFIBER_AWAIT createCompilerObject();
}))

int main(int argc, const char **argv) {
	std::cout << "kernletcc: Starting up" << std::endl;
	{
		async::queue_scope scope{helix::globalQueue()};
		asyncMain(argv);
	}

	helix::globalQueue()->run();

	return 0;
}


