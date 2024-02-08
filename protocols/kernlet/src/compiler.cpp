#include <string.h>

#include <async/mutex.hpp>
#include <async/oneshot-event.hpp>
#include <helix/ipc.hpp>
#include <protocols/kernlet/compiler.hpp>
#include <protocols/mbus/client.hpp>
#include <bragi/helpers-std.hpp>

#include "kernlet.bragi.hpp"

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

		auto filter = mbus_ng::Conjunction{{
			mbus_ng::EqualsFilter{"class", "kernletcc"}
		}};

		auto enumerator = mbus_ng::Instance::global().enumerate(filter);
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
		assert(events.size() == 1);

		auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
		kernletCompilerLane = (co_await entity.getRemoteLane()).unwrap();
	}
}

async::result<helix::UniqueDescriptor> compile(void *code, size_t size,
		std::vector<BindType> bind_types) {
	// Compile the kernlet object.
	managarm::kernlet::CompileRequest req;

	for(auto bt : bind_types) {
		managarm::kernlet::ParameterType proto;
		switch(bt) {
		case BindType::offset: proto = managarm::kernlet::ParameterType::OFFSET; break;
		case BindType::memoryView: proto = managarm::kernlet::ParameterType::MEMORY_VIEW; break;
		case BindType::bitsetEvent: proto = managarm::kernlet::ParameterType::BITSET_EVENT; break;
		default:
			assert(!"Unexpected binding type");
			__builtin_unreachable();
		}
		req.add_bind_types(proto);
	}

	auto [offer, sendHead, sendTail, sendCode, recvResp, pullKernlet]
			= co_await helix_ng::exchangeMsgs(kernletCompilerLane,
		helix_ng::offer(
			helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
			helix_ng::sendBuffer(code, size),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor()
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(sendHead.error());
	HEL_CHECK(sendTail.error());
	HEL_CHECK(sendCode.error());
	HEL_CHECK(recvResp.error());
	HEL_CHECK(pullKernlet.error());

	auto resp = *bragi::parse_head_only<managarm::kernlet::SvrResponse>(recvResp);

	recvResp.reset();
	assert(resp.error() == managarm::kernlet::Error::SUCCESS);

	co_return pullKernlet.descriptor();
}

