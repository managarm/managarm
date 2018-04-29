
#include <string.h>

#include "../common.hpp"
#include "helout.hpp"

HelHandle __mlibc_getPassthrough(int fd);

namespace {

struct HeloutFile : File {
private:
	COFIBER_ROUTINE(expected<size_t>,
	readSome(Process *, void *data, size_t max_length) override, ([=] {
		(void)data;
		(void)max_length;
		assert(!"Not implemented");
	}))
	
	COFIBER_ROUTINE(expected<PollResult>, poll(uint64_t sequence) override, ([=] {
		// TODO: Signal that we are ready to accept output.
		std::cout << "posix: poll() on helout" << std::endl;
		if(!sequence) {
			PollResult result{1, 0, 0};
			COFIBER_RETURN(result);
		}
	}))

	helix::BorrowedDescriptor getPassthroughLane() override {
		return helix::BorrowedDescriptor(__mlibc_getPassthrough(1));
	}

public:
	HeloutFile(std::shared_ptr<FsLink> link)
	: File{StructName::get("helout"), std::move(link)} { }
};

struct HeloutDevice : UnixDevice {
	HeloutDevice()
	: UnixDevice(VfsType::charDevice) {
		assignId({1, 255}); // This minor is not used by Linux.
	}
	
	std::string nodePath() override {
		return "helout";
	}
	
	COFIBER_ROUTINE(FutureMaybe<SharedFilePtr>,
	open(std::shared_ptr<FsLink> link, SemanticFlags semantic_flags) override, ([=] {
		assert(!semantic_flags);
		auto file = smarter::make_shared<HeloutFile>(std::move(link));
		file->setupWeakFile(file);
		COFIBER_RETURN(File::constructHandle(std::move(file)));
	}))
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createHeloutDevice() {
	return std::make_shared<HeloutDevice>();
}

