
#include <iostream>

#include <helix/memory.hpp>
#include <protocols/clock/defs.hpp>
#include <protocols/mbus/client.hpp>
#include <clock.pb.h>

helix::UniqueDescriptor pageMemory;
helix::Mapping pageMapping;

TrackerPage *getPage() {
	return reinterpret_cast<TrackerPage *>(pageMapping.get());
}

COFIBER_ROUTINE(cofiber::no_future, serve(helix::UniqueLane lane),
		([lane = std::move(lane)] () {
	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::clock::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.req_type() == managarm::clock::CntReqType::ACCESS_PAGE) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor send_memory;

			managarm::clock::SvrResponse resp;
			resp.set_error(managarm::clock::Error::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_memory, pageMemory));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else{
			throw std::runtime_error("Unexpected request type");
		}
	}
}))

// ----------------------------------------------------------------
// Freestanding mbus functions.
// ----------------------------------------------------------------

COFIBER_ROUTINE(cofiber::no_future, runObject(), ([] {
	// Create an mbus object for the device.
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();
	
	mbus::Properties descriptor{
		{"class", mbus::StringItem{"clocktracker"}},
	};

	auto handler = mbus::ObjectHandler{}
	.withBind([=] () -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		serve(std::move(local_lane));

		async::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.async_get();
	});

	COFIBER_AWAIT root.createObject("clocktracker", descriptor, std::move(handler));
}))

int main() {
	std::cout << "clocktracker: Starting driver" << std::endl;

	size_t page_size = 4096;

	// Allocate and map our tracker page.
	HelHandle handle;
	HEL_CHECK(helAllocateMemory(page_size, 0, &handle));
	pageMemory = helix::UniqueDescriptor{handle};
	pageMapping = helix::Mapping{pageMemory, 0, page_size};

	// Initialize the tracker page.
	auto page = getPage();
	memset(page, 0, page_size);

	runObject();

	while(true)
		helix::Dispatcher::global().dispatch();
	
	return 0;
}


