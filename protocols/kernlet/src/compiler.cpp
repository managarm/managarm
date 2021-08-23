#include <string.h>

#include <async/mutex.hpp>
#include <async/oneshot-event.hpp>
#include <helix/ipc.hpp>
#include <protocols/kernlet/compiler.hpp>
#include <protocols/mbus/client.hpp>

#include "kernlet.pb.h"

namespace {
	helix::UniqueLane kernletCompilerLane;
	async::mutex enumerationMutex;
	async::oneshot_event foundKernletCompiler;
}

async::result<void> connectKernletCompiler() {
	co_await enumerationMutex.async_lock();
	{
		std::unique_lock lock{enumerationMutex, std::adopt_lock};

		if(kernletCompilerLane)
			co_return;

		auto root = co_await mbus::Instance::global().getRoot();

		auto filter = mbus::Conjunction({
			mbus::EqualsFilter("class", "kernletcc")
		});

		auto handler = mbus::ObserverHandler{}
		.withAttach([] (mbus::Entity entity, mbus::Properties properties) -> async::detached {
			std::cout << "kernlet: Found kernletcc" << std::endl;

			kernletCompilerLane = helix::UniqueLane(co_await entity.bind());
			foundKernletCompiler.raise();
		});

		co_await root.linkObserver(std::move(filter), std::move(handler));
		co_await foundKernletCompiler.wait();
	}
}

async::result<helix::UniqueDescriptor> compile(void *code, size_t size,
		std::vector<BindType> bind_types) {
	// Compile the kernlet object.
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
	auto [offer, send_req, send_code, recv_resp, pull_kernlet]
			= co_await helix_ng::exchangeMsgs(kernletCompilerLane,
		helix_ng::offer(
			helix_ng::sendBuffer(ser.data(), ser.size()),
			helix_ng::sendBuffer(code, size),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor()
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(send_code.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_kernlet.error());

	managarm::kernlet::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	recv_resp.reset();
	assert(resp.error() == managarm::kernlet::Error::SUCCESS);

	co_return pull_kernlet.descriptor();
}

