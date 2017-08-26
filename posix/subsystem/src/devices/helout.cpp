
#include <string.h>

#include "common.hpp"
#include "devices/helout.hpp"

HelHandle __mlibc_getPassthrough(int fd);

namespace {

struct HeloutFile : File {
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

	COFIBER_ROUTINE(FutureMaybe<helix::UniqueDescriptor>, accessMemory() override, ([=] {
		assert(!"Not implemented");
	}))

	helix::BorrowedDescriptor getPassthroughLane() override {
		return helix::BorrowedDescriptor(__mlibc_getPassthrough(1));
	}

public:
	HeloutFile(std::shared_ptr<Node> node)
	: File{std::move(node)} { }
};

struct HeloutDevice : Device {
	static VfsType getType(std::shared_ptr<Device>) {
		return VfsType::charDevice;
	}

	static std::string getName(std::shared_ptr<Device>) {
		return "helout";
	}

	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<File>>,
			open(std::shared_ptr<Device> device, std::shared_ptr<Node> node), ([=] {
		(void)device;
		COFIBER_RETURN(std::make_shared<HeloutFile>(std::move(node)));
	}))

	static const DeviceOperations operations;

	HeloutDevice()
	: Device(&operations) { }
};

const DeviceOperations HeloutDevice::operations{
	&HeloutDevice::getType,
	&HeloutDevice::getName,
	&HeloutDevice::open,
	nullptr
};

} // anonymous namespace

std::shared_ptr<Device> createHeloutDevice() {
	return std::make_shared<HeloutDevice>();
}

