
#include <string.h>

#include "common.hpp"
#include "devices/helout.hpp"

HelHandle __mlibc_getPassthrough(int fd);

namespace {

struct HeloutFile : ProxyFile {
private:
	COFIBER_ROUTINE(FutureMaybe<off_t>, seek(off_t offset, VfsSeek whence) override, ([=] {
		(void)offset;
		(void)whence;
		assert(!"Not implemented");
	}))

	COFIBER_ROUTINE(FutureMaybe<size_t>, readSome(void *data, size_t max_length) override, ([=] {
		(void)data;
		(void)max_length;
		assert(!"Not implemented");
	}))

	helix::BorrowedDescriptor getPassthroughLane() override {
		return helix::BorrowedDescriptor(__mlibc_getPassthrough(1));
	}

public:
	HeloutFile(std::shared_ptr<FsLink> link)
	: ProxyFile{std::move(link)} { }
};

struct HeloutDevice : UnixDevice {
	HeloutDevice()
	: UnixDevice(VfsType::charDevice) { }
	
	std::string getName() override {
		return "helout";
	}
	
	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<File>>,
			open(std::shared_ptr<FsLink> link) override, ([=] {
		COFIBER_RETURN(std::make_shared<HeloutFile>(std::move(link)));
	}))
};

} // anonymous namespace

std::shared_ptr<UnixDevice> createHeloutDevice() {
	return std::make_shared<HeloutDevice>();
}

