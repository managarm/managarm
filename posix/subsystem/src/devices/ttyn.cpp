#include <asm/ioctls.h>
#include <string.h>
#include <sys/epoll.h>

#include "../common.hpp"
#include "ttyn.hpp"
#include <bragi/helpers-std.hpp>

#include <bitset>

namespace {

struct TTYNFile final : File {
private:
	async::result<std::expected<size_t, Error>>	
	readSome(Process *, void *data, size_t length, async::cancellation_token) override {
		memset(data, 0, length);
		co_return length;
	}

	async::result<frg::expected<Error, size_t>> writeAll(Process *, const void *, size_t length) override {
		co_return length;
	}

	async::result<frg::expected<Error, off_t>> seek(off_t, VfsSeek) override {
		co_return 0;
	}

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t sequence, int mask,
			async::cancellation_token cancellation) override {
		(void)mask;

		if(sequence > 1)
			co_return Error::illegalArguments;

		if(sequence)
			co_await async::suspend_indefinitely(cancellation);
		co_return PollWaitResult{1, EPOLLOUT};
	}

	async::result<frg::expected<Error, PollStatusResult>>
	pollStatus(Process *) override {
		co_return PollStatusResult{1, EPOLLOUT};
	}

	async::result<void>
	ioctl(Process *, uint32_t id, helix_ng::RecvInlineResult msg, helix::UniqueLane conversation) override {
		if(id == managarm::fs::GenericIoctlRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::fs::GenericIoctlRequest>(msg);
			assert(req);

			if(req->command() == TIOCSCTTY) {
				auto [extractCreds] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::extractCredentials()
				);
				HEL_CHECK(extractCreds.error());

				managarm::fs::GenericIoctlReply resp;
				resp.set_error(managarm::fs::Errors::SUCCESS);

				auto ser = resp.SerializeAsString();
				auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);
				HEL_CHECK(sendResp.error());
			}else{
				std::cout << "\e[31m" "posix: Rejecting unknown PTS ioctl (commonIoctl) " << req->command()
						<< "\e[39m" << std::endl;
			}
		}else{
			std::cout << "\e[31m" "posix: Rejecting unknown PTS ioctl message (commonIoctl) " << id
					<< "\e[39m" << std::endl;
		}
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

public:
	static void serve(smarter::shared_ptr<TTYNFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	TTYNFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
	: File{FileKind::unknown,  StructName::get("tty1-file"), std::move(mount), std::move(link)} { }
};

struct TTYNDevice final : UnixDevice {
	TTYNDevice(int n_)
	: UnixDevice(VfsType::charDevice) {
		n = n_;
		assignId({4, n_});
	}

	std::string nodePath() override {
		return "tty" + std::to_string(n);
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(Process *, std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
			std::cout << "\e[31mposix: TTYNFile open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}

		auto file = smarter::make_shared<TTYNFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		TTYNFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}
private:
	int n;
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createTTYNDevice(int n) {
	return std::make_shared<TTYNDevice>(n);
}
