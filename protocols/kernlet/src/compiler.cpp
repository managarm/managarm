
#include <string.h>

#include <async/jump.hpp>
#include <helix/ipc.hpp>
#include <protocols/kernlet/compiler.hpp>
#include <protocols/mbus/client.hpp>

#include "kernlet.pb.h"

helix::UniqueLane kernletCompilerLane;
async::jump foundKernletCompiler;

async::result<void> connectKernletCompiler() {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("class", "kernletcc")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) -> async::detached {
		std::cout << "kernlet: Found kernletcc" << std::endl;

		kernletCompilerLane = helix::UniqueLane(co_await entity.bind());
		foundKernletCompiler.trigger();
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
	co_await foundKernletCompiler.async_wait();
}

async::result<helix::UniqueDescriptor> compile(void *code, size_t size,
		std::vector<BindType> bind_types) {
	// Compile the kernlet object.
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::SendBuffer send_code;
	helix::RecvInline recv_resp;
	helix::PullDescriptor pull_kernlet;

	managarm::kernlet::CntRequest req;
	req.set_req_type(managarm::kernlet::CntReqType::COMPILE);

	for(auto bt : bind_types) {
		managarm::kernlet::ParameterType proto;
		switch(bt) {
		case BindType::offset: proto = managarm::kernlet::OFFSET; break;
		case BindType::memoryView: proto = managarm::kernlet::MEMORY_VIEW; break;
		case BindType::bitsetEvent: proto = managarm::kernlet::BITSET_EVENT; break;
		default:
			assert(!"Unexpected binding type");
			__builtin_unreachable();
		}
		req.add_bind_types(proto);
	}

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(kernletCompilerLane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&send_code, code, size, kHelItemChain),
			helix::action(&recv_resp, kHelItemChain),
			helix::action(&pull_kernlet));
	co_await transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(send_code.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_kernlet.error());

	managarm::kernlet::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::kernlet::Error::SUCCESS);

	co_return pull_kernlet.descriptor();
}

