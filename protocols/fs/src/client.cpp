
#include <iostream>

#include "fs.bragi.hpp"
#include "protocols/fs/client.hpp"

namespace protocols {
namespace fs {

File::File(helix::UniqueDescriptor lane)
: _lane(std::move(lane)) { }

async::result<void> File::seekAbsolute(int64_t offset) {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::SEEK_ABS);
	req.set_rel_offset(offset);

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];

	auto [offer, send_req, recv_resp] =
		co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvBuffer(buffer, 128)
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
}

async::result<size_t> File::readSome(void *data, size_t max_length) {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::READ);
	req.set_size(max_length);

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];

	auto [offer, send_req, imbue_creds, recv_resp, recv_data] =
		co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::imbueCredentials(),
				helix_ng::recvBuffer(buffer, 128),
				helix_ng::recvBuffer(data, max_length)
			)
		);

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

async::result<size_t> File::writeSome(const void *data, size_t maxLength) {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::WRITE);
	req.set_size(maxLength);

	auto ser = req.SerializeAsString();

	auto [offer, sendReq, imbueCreds, sendData, recvResp] =
		co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::imbueCredentials(),
				helix_ng::sendBuffer(data, maxLength),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(imbueCreds.error());
	HEL_CHECK(sendData.error());
	HEL_CHECK(recvResp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(recvResp.data(), recvResp.length());
	recvResp.reset();
	if(resp.error() == managarm::fs::Errors::END_OF_FILE)
		co_return 0;
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
	co_return resp.size();
}

async::result<frg::expected<Error, PollWaitResult>> File::pollWait(uint64_t sequence, int mask,
		async::cancellation_token cancellation) {
	HelHandle cancel_handle;
	HEL_CHECK(helCreateOneshotEvent(&cancel_handle));
	helix::UniqueDescriptor cancel_event{cancel_handle};

	async::cancellation_callback cancel_cb{cancellation, [&] {
		std::cerr << "\e[33mprotocols/fs: poll() was cancelled on client-side\e[39m" << std::endl;
		HEL_CHECK(helRaiseEvent(cancel_event.getHandle()));
	}};

	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::FILE_POLL_WAIT);
	req.set_sequence(sequence);
	req.set_event_mask(mask);

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];

	auto [offer, send_req, push_cancel, recv_resp] =
		co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(cancel_event),
				helix_ng::recvBuffer(buffer, 128)
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(push_cancel.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());

	if(resp.error() != managarm::fs::Errors::SUCCESS)
		co_return static_cast<Error>(resp.error());
	co_return PollWaitResult(resp.sequence(), resp.edges());
}

async::result<frg::expected<Error, PollStatusResult>> File::pollStatus() {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::FILE_POLL_STATUS);

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];

	auto [offer, send_req, recv_resp] =
		co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvBuffer(buffer, 128)
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());

	if(resp.error() != managarm::fs::Errors::SUCCESS)
		co_return static_cast<Error>(resp.error());
	co_return PollStatusResult(resp.sequence(), resp.status());
}

async::result<helix::UniqueDescriptor> File::accessMemory() {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::MMAP);

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];

	auto [offer, send_req, recv_resp, recv_memory] =
		co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvBuffer(buffer, 128),
				helix_ng::pullDescriptor()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(recv_memory.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
	co_return recv_memory.descriptor();
}

async::result<frg::expected<Error, File>> File::createSocket(helix::BorrowedLane lane,
		int domain, int type, int proto, int flags) {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::CREATE_SOCKET);
	req.set_domain(domain);
	req.set_type(type);
	req.set_protocol(proto);
	req.set_flags(flags);

	auto [offer, send_req, recv_resp, recv_lane] = co_await helix_ng::exchangeMsgs(
		lane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor()
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(recv_lane.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	if(resp.error() != managarm::fs::Errors::SUCCESS)
		co_return static_cast<Error>(resp.error());

	co_return File{recv_lane.descriptor()};
}

async::result<Error> File::connect(const struct sockaddr *addr_ptr, socklen_t addr_length) {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::PT_CONNECT);

	auto [offer, send_req, imbue_creds, send_addr, recv_resp] = co_await helix_ng::exchangeMsgs(
	    _lane,
	    helix_ng::offer(
	        helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
	        helix_ng::imbueCredentials(),
	        helix_ng::sendBuffer(const_cast<struct sockaddr *>(addr_ptr), addr_length),
	        helix_ng::recvInline()
	    )
	);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(imbue_creds.error());
	HEL_CHECK(send_addr.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());

	co_return static_cast<Error>(resp.error());
}

async::result<frg::expected<Error, size_t>>
File::sendto(const void *buf, size_t len, int flags, const struct sockaddr *addr_ptr, socklen_t addr_length) {
	managarm::fs::SendMsgRequest req;
	req.set_flags(flags);
	req.set_size(len);
	req.set_has_cmsg_creds(false);
	req.set_has_cmsg_rights(false);

	auto [offer, send_head, send_tail, send_data, imbue_creds, send_addr, recv_resp] =
	    co_await helix_ng::exchangeMsgs(
	        _lane,
	        helix_ng::offer(
	            helix_ng::sendBragiHeadTail(req, frg::stl_allocator{}),
	            helix_ng::sendBuffer(buf, len),
	            helix_ng::imbueCredentials(),
	            helix_ng::sendBuffer(addr_ptr, addr_length),
	            helix_ng::recvInline()
	        )
	    );

	HEL_CHECK(offer.error());
	HEL_CHECK(send_head.error());
	HEL_CHECK(send_tail.error());
	HEL_CHECK(send_data.error());
	HEL_CHECK(imbue_creds.error());
	HEL_CHECK(send_addr.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::SendMsgReply resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());

	if(resp.error() != managarm::fs::Errors::SUCCESS)
		co_return static_cast<Error>(resp.error());

	co_return resp.size();
}

async::result<frg::expected<Error, size_t>>
File::recvfrom(void *buf, size_t len, int flags, struct sockaddr *addr_ptr, socklen_t addr_length) {
	managarm::fs::RecvMsgRequest req;
	req.set_flags(flags);
	req.set_size(len);
	req.set_addr_size(addr_length);
	req.set_ctrl_size(0);

	auto [offer, send_req, imbue_creds, recv_resp, recv_addr, recv_data, recv_ctrl] =
	    co_await helix_ng::exchangeMsgs(
	        _lane,
	        helix_ng::offer(
	            helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
	            helix_ng::imbueCredentials(),
	            helix_ng::recvInline(),
	            helix_ng::recvBuffer(reinterpret_cast<void *>(addr_ptr), addr_length),
	            helix_ng::recvBuffer(buf, len),
	            helix_ng::recvBuffer(nullptr, 0)
	        )
	    );

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(imbue_creds.error());
	HEL_CHECK(recv_resp.error());

	managarm::fs::RecvMsgReply resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());

	if(resp.error() != managarm::fs::Errors::SUCCESS)
		co_return static_cast<Error>(resp.error());

	co_return resp.ret_val();
}

} } // namespace protocol::fs

