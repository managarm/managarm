
#include <string.h>

#include "../common.hpp"
#include "helout.hpp"

#include <experimental/coroutine>

HelHandle __mlibc_getPassthrough(int fd);

namespace {

struct HeloutFile final : File {
private:
	expected<size_t> readSome(Process *, void *data, size_t max_length) override {
		(void)data;
		(void)max_length;
		assert(!"Not implemented");
		__builtin_unreachable();
	}
	
	expected<PollResult> poll(Process *, uint64_t sequence, async::cancellation_token) override {
		// TODO: Signal that we are ready to accept output.
		std::cout << "posix: poll() on helout" << std::endl;
		if(!sequence) {
			PollResult result{1, 0, 0};
			co_return result;
		}

		co_await std::experimental::suspend_always{};
		__builtin_unreachable();
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
	
	FutureMaybe<SharedFilePtr> open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		assert(!(semantic_flags & ~(semanticRead | semanticWrite)));
		auto file = smarter::make_shared<HeloutFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		co_return File::constructHandle(std::move(file));
	}
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createHeloutDevice() {
	return std::make_shared<HeloutDevice>();
}

