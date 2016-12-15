
#include <string.h>

#include "common.hpp"
#include "device.hpp"

DeviceManager deviceManager;

HelHandle __mlibc_getPassthrough(int fd);

// --------------------------------------------------------
// Device
// --------------------------------------------------------

FutureMaybe<std::shared_ptr<File>> open(std::shared_ptr<Device> device) {
	return device->operations()->open(device);
}

// --------------------------------------------------------
// DeviceManager
// --------------------------------------------------------

namespace {

struct HeloutFile : File {
private:
	static COFIBER_ROUTINE(FutureMaybe<off_t>, seek(std::shared_ptr<File> object,
			off_t offset, VfsSeek whence), ([=] {
		(void)object;
		(void)offset;
		(void)whence;
		assert(!"Not implemented");
	}))

	static COFIBER_ROUTINE(FutureMaybe<size_t>, readSome(std::shared_ptr<File> object,
			void *data, size_t max_length), ([=] {
		(void)object;
		(void)data;
		(void)max_length;
		assert(!"Not implemented");
	}))

	static COFIBER_ROUTINE(FutureMaybe<helix::UniqueDescriptor>,
			accessMemory(std::shared_ptr<File> object), ([=] {
		(void)object;
		assert(!"Not implemented");
	}))

	static helix::BorrowedDescriptor getPassthroughLane(std::shared_ptr<File> object) {
		(void)object;
		return helix::BorrowedDescriptor(__mlibc_getPassthrough(1));
	}

	static const FileOperations operations;

public:
	HeloutFile()
	: File(&operations) { }
};
	
const FileOperations HeloutFile::operations{
	&HeloutFile::seek,
	&HeloutFile::readSome,
	&HeloutFile::accessMemory,
	&HeloutFile::getPassthroughLane
};

struct HeloutDevice : Device {
	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<File>>,
			open(std::shared_ptr<Device> device), ([=] {
		(void)device;
		COFIBER_RETURN(std::make_shared<HeloutFile>());
	}))

	static const DeviceOperations operations;

	HeloutDevice()
	: Device({0, 1}, &operations) { }
};

const DeviceOperations HeloutDevice::operations{
	&HeloutDevice::open
};

} // anonymous namespace

std::shared_ptr<Device> DeviceManager::get(DeviceId id) {
	(void)id; // TODO: implement proper device allocation.
	return std::make_shared<HeloutDevice>();
}

