
#include <unistd.h>
#include <fcntl.h>

#include <protocols/fs/client.hpp>
#include "common.hpp"
#include "extern_fs.hpp"

HelHandle __mlibc_getPassthrough(int fd);
HelHandle __raw_map(int fd);

namespace extern_fs {

namespace {

struct OpenFile {
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

struct RegularNode {
	RegularNode(int fd)
	: _fd(fd) { }

	COFIBER_ROUTINE(FutureMaybe<SharedFile>, open(SharedEntry entry), ([=] {
		COFIBER_RETURN(SharedFile::create<OpenFile>(std::move(entry), _fd));
	}))

private:
	int _fd;
};

struct DirectoryNode {
	COFIBER_ROUTINE(FutureMaybe<SharedNode>, resolveChild(std::string name), ([=] {
		int fd = open(name.c_str(), O_RDONLY);
		COFIBER_RETURN(SharedNode::createRegular<RegularNode>(fd));
	}))
};

} // anonymous namespace

SharedNode createRootNode() {
	return SharedNode::createDirectory<DirectoryNode>();
}

} // namespace extern_fs

