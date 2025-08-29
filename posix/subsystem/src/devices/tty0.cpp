#include <string.h>
#include <sys/epoll.h>

#include "../common.hpp"
#include "zero.hpp"

#include <bitset>
#include <coroutine>

namespace {

struct TTY0File final : File {
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

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

public:
	static void serve(smarter::shared_ptr<TTY0File> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	TTY0File(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
	: File{FileKind::unknown,  StructName::get("tty0-file"), std::move(mount), std::move(link)} { }
};

struct TTY0Device final : UnixDevice {
	TTY0Device()
	: UnixDevice(VfsType::charDevice) {
		assignId({4, 0});
	}

	std::string nodePath() override {
		return "tty0";
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
			std::cout << "\e[31mposix: TTY0File open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}

		auto file = smarter::make_shared<TTY0File>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		TTY0File::serve(file);
		co_return File::constructHandle(std::move(file));
	}
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createTTY0Device() {
	return std::make_shared<TTY0Device>();
}
