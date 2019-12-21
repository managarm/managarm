
#include <iostream>

#include "fs.pb.h"
#include "protocols/fs/client.hpp"

namespace protocols {
namespace fs {

File::File(helix::UniqueDescriptor lane)
: _lane(std::move(lane)) { }

async::result<void> File::seekAbsolute(int64_t offset) {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvBuffer recv_resp;

	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::SEEK_ABS);
	req.set_rel_offset(offset);

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];
	auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, buffer, 128));
	co_await transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
}

async::result<size_t> File::readSome(void *data, size_t max_length) {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::ImbueCredentials imbue_creds;
	helix::RecvBuffer recv_resp;
	helix::RecvBuffer recv_data;

	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::READ);
	req.set_size(max_length);

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];
	auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&imbue_creds, kHelItemChain),
			helix::action(&recv_resp, buffer, 128, kHelItemChain),
			helix::action(&recv_data, data, max_length));
	co_await transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(imbue_creds.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(recv_data.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	if(resp.error() == managarm::fs::Errors::END_OF_FILE) {
		co_return 0;
	}
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
	co_return recv_data.actualLength();
}

async::result<PollResult> File::poll(uint64_t sequence,
		async::cancellation_token cancellation) {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::PushDescriptor push_cancel;
	helix::RecvBuffer recv_resp;

	HelHandle cancel_handle;
	HEL_CHECK(helCreateOneshotEvent(&cancel_handle));
	helix::UniqueDescriptor cancel_event{cancel_handle};

	async::cancellation_callback cancel_cb{cancellation, [&] {
		std::cerr << "\e[33mprotocols/fs: poll() was cancelled on client-side\e[39m" << std::endl;
		HEL_CHECK(helRaiseEvent(cancel_event.getHandle()));
	}};

	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::FILE_POLL);
	req.set_sequence(sequence);

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];
	auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&push_cancel, cancel_event, kHelItemChain),
			helix::action(&recv_resp, buffer, 128));
	co_await transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(push_cancel.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);

	PollResult result{resp.sequence(), resp.edges(), resp.status()};
	co_return result;
}

async::result<helix::UniqueDescriptor> File::accessMemory(off_t offset) {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvBuffer recv_resp;
	helix::PullDescriptor recv_memory;

	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::MMAP);
	req.set_rel_offset(offset);

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];
	auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, buffer, 128, kHelItemChain),
			helix::action(&recv_memory));
	co_await transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(recv_memory.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
	co_return recv_memory.descriptor();
}

} } // namespace protocol::fs

