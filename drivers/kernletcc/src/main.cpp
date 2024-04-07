
#include <fcntl.h>
#include <sys/stat.h>
#include <iostream>

#include <async/oneshot-event.hpp>
#include <helix/memory.hpp>
#include <protocols/mbus/client.hpp>
#include <bragi/helpers-std.hpp>
#include <kernlet.bragi.hpp>
#include "common.hpp"

static bool dumpHex = false;

// ----------------------------------------------------------------------------
// kernletctl handling.
// ----------------------------------------------------------------------------

helix::UniqueLane kernletCtlLane;
async::oneshot_event foundKernletCtl;

async::result<void> enumerateCtl() {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("class", "kernletctl")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties) -> async::detached {
		std::cout << "kernletcc: Found kernletctl" << std::endl;

		kernletCtlLane = helix::UniqueLane(co_await entity.bind());
		foundKernletCtl.raise();
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
	co_await foundKernletCtl.wait();
}

async::result<helix::UniqueDescriptor> upload(const void *elf, size_t size,
		std::vector<BindType> bind_types) {
	managarm::kernlet::UploadRequest req;

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

	auto ser = req.SerializeAsString();
	auto [offer, sendHead, sendTail, sendData, recvResp, pullKernlet] = co_await helix_ng::exchangeMsgs(
		kernletCtlLane,
		helix_ng::offer(
			helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
			helix_ng::sendBuffer(elf, size),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor())
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(sendHead.error());
	HEL_CHECK(sendTail.error());
	HEL_CHECK(sendData.error());
	HEL_CHECK(recvResp.error());
	HEL_CHECK(pullKernlet.error());

	auto resp = *bragi::parse_head_only<managarm::kernlet::SvrResponse>(recvResp);

	recvResp.reset();
	assert(resp.error() == managarm::kernlet::Error::SUCCESS);
	std::cout << "kernletcc: Upload success" << std::endl;

	co_return pullKernlet.descriptor();
}

// ----------------------------------------------------------------------------
// kernletcc mbus interface.
// ----------------------------------------------------------------------------

async::detached serveCompiler(helix::UniqueLane lane) {
	while(true) {
		auto [accept, recvHead] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::accept(
				helix_ng::recvInline())
		);
		if(accept.error() == kHelErrEndOfLane) {
			std::cout << "kernletcc: Client closed its connection" << std::endl;
			co_return;
		}
		HEL_CHECK(accept.error());
		HEL_CHECK(recvHead.error());

		auto conversation = accept.descriptor();

		auto preamble = bragi::read_preamble(recvHead);
		assert(!preamble.error());

		std::vector<std::byte> tailBuffer(preamble.tail_size());
		auto [recvTail, recvCode] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size()),
			helix_ng::recvInline() // TODO(qookie): What about kernlets larger than 128 bytes?
		);

		HEL_CHECK(recvTail.error());
		HEL_CHECK(recvCode.error());

		if(preamble.id() == bragi::message_id<managarm::kernlet::CompileRequest>) {
			auto maybeReq = bragi::parse_head_tail<managarm::kernlet::CompileRequest>(recvHead, tailBuffer);

			if (!maybeReq) {
				std::cout << "kernletcc: Ignoring request due to decoding failure.\n";
				continue;
			}

			auto &req = *maybeReq;

			std::vector<BindType> bind_types;
			for(size_t i = 0; i < req.bind_types_size(); i++) {
				auto proto = req.bind_types(i);
				BindType bt;
				switch(proto) {
				case managarm::kernlet::ParameterType::OFFSET: bt = BindType::offset; break;
				case managarm::kernlet::ParameterType::MEMORY_VIEW: bt = BindType::memoryView; break;
				case managarm::kernlet::ParameterType::BITSET_EVENT: bt = BindType::bitsetEvent; break;
				default:
					assert(!"Unexpected binding type");
				}
				bind_types.push_back(bt);
			}

			auto elf = compileFafnir(reinterpret_cast<const uint8_t *>(recvCode.data()),
					recvCode.length(), bind_types);
			recvCode.reset();

			if(dumpHex) {
				for(size_t i = 0; i < elf.size(); i++) {
					printf("%02x", elf[i]);
					if((i % 32) == 31)
						putchar('\n');
					else if((i % 8) == 7)
						putchar(' ');
				}
				putchar('\n');
			}

			auto object = co_await upload(elf.data(), elf.size(), bind_types);

			managarm::kernlet::SvrResponse resp;
			resp.set_error(managarm::kernlet::Error::SUCCESS);

			auto [sendResp, pushKernlet] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::pushDescriptor(object)
			);
			if(sendResp.error() == kHelErrEndOfLane) {
				std::cout << "\e[31m" "kernletcc: Client unexpectedly closed its connection"
						"\e[39m" << std::endl;
				co_return;
			}
			HEL_CHECK(sendResp.error());
			HEL_CHECK(pushKernlet.error());
		}else{
			throw std::runtime_error("Unexpected request type");
		}
	}
}

async::result<void> createCompilerObject() {
	// Create an mbus object for the device.
	auto root = co_await mbus::Instance::global().getRoot();

	mbus::Properties descriptor{
		{"class", mbus::StringItem{"kernletcc"}},
	};

	auto handler = mbus::ObjectHandler{}
	.withBind([=] () -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		serveCompiler(std::move(local_lane));

		co_return std::move(remote_lane);
	});

	co_await root.createObject("kernletcc", descriptor, std::move(handler));
}

// ----------------------------------------------------------------
// Freestanding mbus functions.
// ----------------------------------------------------------------

async::detached asyncMain(const char **args) {
	(void) args;

	co_await enumerateCtl();
	co_await createCompilerObject();
}

int main(int, const char **argv) {
	std::cout << "kernletcc: Starting up" << std::endl;

	asyncMain(argv);
	async::run_forever(helix::currentDispatcher);
}

