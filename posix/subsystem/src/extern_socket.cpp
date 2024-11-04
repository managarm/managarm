#include "extern_socket.hpp"

#include "fs.bragi.hpp"
#include "protocols/fs/client.hpp"

namespace {
struct Socket : File {
	Socket(helix::UniqueLane sockLane)
	    : File{StructName::get("extern-socket")},
	      _file{std::move(sockLane)} {}

	async::result<frg::expected<Error, PollWaitResult>> pollWait(
	    Process *, uint64_t sequence, int mask, async::cancellation_token cancellation
	) override {
		auto resultOrError = co_await _file.pollWait(sequence, mask, cancellation);
		assert(resultOrError);
		co_return resultOrError.value();
	}

	async::result<frg::expected<Error, PollStatusResult>> pollStatus(Process *) override {
		auto resultOrError = co_await _file.pollStatus();
		assert(resultOrError);
		co_return resultOrError.value();
	}

	helix::BorrowedDescriptor getPassthroughLane() override { return _file.getLane(); }

  private:
	protocols::fs::File _file;
};
} // namespace

namespace extern_socket {

async::result<smarter::shared_ptr<File, FileHandle>>
createSocket(helix::BorrowedLane lane, int domain, int type, int proto, int flags) {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::CREATE_SOCKET);
	req.set_domain(domain);
	req.set_type(type);
	req.set_protocol(proto);
	req.set_flags(flags);

	auto req_data = req.SerializeAsString();
	char buffer[128];

	auto [offer, send_req, recv_resp, recv_lane] = co_await helix_ng::exchangeMsgs(
	    lane,
	    helix_ng::offer(
	        helix_ng::sendBuffer(req_data.data(), req_data.size()),
	        helix_ng::recvBuffer(buffer, sizeof(buffer)),
	        helix_ng::pullDescriptor()
	    )
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(recv_lane.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);

	auto file = smarter::make_shared<Socket>(recv_lane.descriptor());
	file->setupWeakFile(file);
	co_return File::constructHandle(file);
}

} // namespace extern_socket
