#include <string.h>
#include <sys/epoll.h>

#include "../common.hpp"
#include "helout.hpp"

#include <bitset>
#include <coroutine>

HelHandle __mlibc_getPassthrough(int fd);

namespace {

struct HeloutFile final : File {
private:
	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t max_length) override {
		(void)data;
		(void)max_length;
		assert(!"Not implemented");
		__builtin_unreachable();
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
		return helix::BorrowedDescriptor(__mlibc_getPassthrough(1));
	}

public:
	HeloutFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
	: File{StructName::get("helout"), std::move(mount), std::move(link),
			File::defaultIsTerminal} { }
};

struct HeloutDevice final : UnixDevice {
	HeloutDevice()
	: UnixDevice(VfsType::charDevice) {
		assignId({1, 255}); // This minor is not used by Linux.
	}

	std::string nodePath() override {
		return "helout";
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		if(semantic_flags & ~(semanticRead | semanticWrite)){
			std::cout << "\e[31mposix: open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}

		auto file = smarter::make_shared<HeloutFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		co_return File::constructHandle(std::move(file));
	}
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createHeloutDevice() {
	return std::make_shared<HeloutDevice>();
}

