
#include <unistd.h>
#include <fcntl.h>

#include <protocols/fs/client.hpp>
#include "common.hpp"
#include "extern_fs.hpp"

HelHandle __mlibc_getPassthrough(int fd);
HelHandle __raw_map(int fd);

namespace extern_fs {

namespace {

struct OpenFile : FileData {
	OpenFile(int fd)
	: _file(helix::UniqueDescriptor(__mlibc_getPassthrough(fd))) { }

	COFIBER_ROUTINE(FutureMaybe<off_t>, seek(off_t offset, VfsSeek whence), ([=] {
		assert(whence == VfsSeek::absolute);
		COFIBER_AWAIT _file.seekAbsolute(offset);
		COFIBER_RETURN(offset);
	}))

	COFIBER_ROUTINE(FutureMaybe<size_t>, readSome(void *data, size_t max_length), ([=] {
		size_t length = COFIBER_AWAIT _file.readSome(data, max_length);
		COFIBER_RETURN(length);
	}))

	COFIBER_ROUTINE(FutureMaybe<helix::UniqueDescriptor>, accessMemory(), ([=] {
		auto memory = COFIBER_AWAIT _file.accessMemory();
		COFIBER_RETURN(std::move(memory));
	}))

	helix::BorrowedDescriptor getPassthroughLane() {
		return _file.getLane();
	}

private:
	protocols::fs::File _file;
};

struct Regular : RegularData {
	Regular(int fd)
	: _fd(fd) { }

	COFIBER_ROUTINE(FutureMaybe<SharedFile>, open(), ([=] {
		COFIBER_RETURN(SharedFile{std::make_shared<OpenFile>(_fd)});
	}))

private:
	int _fd;
};

struct Directory : TreeData {
	COFIBER_ROUTINE(FutureMaybe<SharedLink>, mkdir(std::string name), ([=] {
		(void)name;
		assert(!"mkdir is not implemented for extern_fs");
	}))
	
	COFIBER_ROUTINE(FutureMaybe<SharedLink>, symlink(std::string name, std::string link), ([=] {
		(void)name;
		(void)link;
		assert(!"symlink is not implemented for extern_fs");
	}))

	COFIBER_ROUTINE(FutureMaybe<SharedLink>, getLink(std::string name), ([=] {
		int fd = open(name.c_str(), O_RDONLY);
		auto node = SharedNode{std::make_shared<Regular>(fd)};
		// TODO: do not use createRoot here!
		COFIBER_RETURN(SharedLink::createRoot(std::move(node)));
	}))
};

} // anonymous namespace

SharedLink createRoot() {
	auto node = SharedNode{std::make_shared<Directory>()};
	return SharedLink::createRoot(std::move(node));
}

} // namespace extern_fs

