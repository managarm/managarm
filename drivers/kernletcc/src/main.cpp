
#include <fcntl.h>
#include <sys/stat.h>
#include <iostream>

#include <async/oneshot-event.hpp>
#include <helix/memory.hpp>
#include <protocols/mbus/client.hpp>
#include <kernlet.pb.h>
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
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) -> async::detached {
		std::cout << "kernletcc: Found kernletctl" << std::endl;

		kernletCtlLane = helix::UniqueLane(co_await entity.bind());
		foundKernletCtl.raise();
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
	co_await foundKernletCtl.wait();
}

async::result<helix::UniqueDescriptor> upload(const void *elf, size_t size,
		std::vector<BindType> bind_types) {
	managarm::kernlet::CntRequest req;
	req.set_req_type(managarm::kernlet::CntReqType::UPLOAD);

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
	auto [offer, send_req, send_data, recv_resp, pull_kernlet] = co_await helix_ng::exchangeMsgs(
		kernletCtlLane,
		helix_ng::offer(
			helix_ng::sendBuffer(ser.data(), ser.size()),
			helix_ng::sendBuffer(elf, size),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor())
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(send_data.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_kernlet.error());

	managarm::kernlet::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::kernlet::Error::SUCCESS);
	std::cout << "kernletcc: Upload success" << std::endl;

	co_return pull_kernlet.descriptor();
}

// ----------------------------------------------------------------------------
// kernletcc mbus interface.
// ----------------------------------------------------------------------------

async::detached serveCompiler(helix::UniqueLane lane) {
	while(true) {
		auto [accept, recv_req, recv_code] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::accept(
				helix_ng::recvInline(),
				helix_ng::recvInline())
		);
		if(accept.error() == kHelErrEndOfLane) {
			std::cout << "kernletcc: Client closed its connection" << std::endl;
			co_return;
		}
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		HEL_CHECK(recv_code.error());

		auto conversation = accept.descriptor();

		managarm::kernlet::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.req_type() == managarm::kernlet::CntReqType::COMPILE) {
			std::vector<BindType> bind_types;
			for(int i = 0; i < req.bind_types_size(); i++) {
				auto proto = req.bind_types(i);
				BindType bt;
				switch(proto) {
				case managarm::kernlet::OFFSET: bt = BindType::offset; break;
				case managarm::kernlet::MEMORY_VIEW: bt = BindType::memoryView; break;
				case managarm::kernlet::BITSET_EVENT: bt = BindType::bitsetEvent; break;
				default:
					assert(!"Unexpected binding type");
				}
				bind_types.push_back(bt);
			}

			auto elf = compileFafnir(reinterpret_cast<const uint8_t *>(recv_code.data()),
					recv_code.length(), bind_types);

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

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_kernlet] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(object)
			);
			if(send_resp.error() == kHelErrEndOfLane) {
				std::cout << "\e[31m" "kernletcc: Client unexpectedly closed its connection"
						"\e[39m" << std::endl;
				co_return;
			}
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_kernlet.error());
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

		async::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.async_get();
	});

	co_await root.createObject("kernletcc", descriptor, std::move(handler));
}

// ----------------------------------------------------------------
// Freestanding mbus functions.
// ----------------------------------------------------------------

async::detached asyncMain(const char **args) {
	co_await enumerateCtl();
	co_await createCompilerObject();
}

int main(int argc, const char **argv) {
	std::cout << "kernletcc: Starting up" << std::endl;
	{
		async::queue_scope scope{helix::globalQueue()};
		asyncMain(argv);
	}

	async::run_forever(helix::globalQueue()->run_token(), helix::currentDispatcher);

	return 0;
}


