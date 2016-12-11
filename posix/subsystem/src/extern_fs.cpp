
#include <unistd.h>
#include <fcntl.h>

#include <protocols/fs/client.hpp>
#include "common.hpp"
#include "extern_fs.hpp"

HelHandle __mlibc_getPassthrough(int fd);
HelHandle __raw_map(int fd);

namespace extern_fs {

namespace {

struct OpenFile : File {
private:
	static COFIBER_ROUTINE(FutureMaybe<off_t>, seek(std::shared_ptr<File> object,
			off_t offset, VfsSeek whence), ([=] {
		auto derived = std::static_pointer_cast<OpenFile>(object);
		assert(whence == VfsSeek::absolute);
		COFIBER_AWAIT derived->_file.seekAbsolute(offset);
		COFIBER_RETURN(offset);
	}))

	static COFIBER_ROUTINE(FutureMaybe<size_t>, readSome(std::shared_ptr<File> object,
			void *data, size_t max_length), ([=] {
		auto derived = std::static_pointer_cast<OpenFile>(object);
		size_t length = COFIBER_AWAIT derived->_file.readSome(data, max_length);
		COFIBER_RETURN(length);
	}))

	static COFIBER_ROUTINE(FutureMaybe<helix::UniqueDescriptor>,
			accessMemory(std::shared_ptr<File> object), ([=] {
		auto derived = std::static_pointer_cast<OpenFile>(object);
		auto memory = COFIBER_AWAIT derived->_file.accessMemory();
		COFIBER_RETURN(std::move(memory));
	}))

	static helix::BorrowedDescriptor getPassthroughLane(std::shared_ptr<File> object) {
		auto derived = std::static_pointer_cast<OpenFile>(object);
		return derived->_file.getLane();
	}

	static const FileOperations operations;

public:
	OpenFile(int fd)
	: File(&operations), _file(helix::UniqueDescriptor(__mlibc_getPassthrough(fd))) { }

private:
	protocols::fs::File _file;
};
	
const FileOperations OpenFile::operations{
	&OpenFile::seek,
	&OpenFile::readSome,
	&OpenFile::accessMemory,
	&OpenFile::getPassthroughLane
};

struct Regular : Node {
private:
	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<File>>,
			open(std::shared_ptr<Node> object), ([=] {
		auto derived = std::static_pointer_cast<Regular>(object);
		COFIBER_RETURN(std::make_shared<OpenFile>(derived->_fd));
	}))

	static const NodeOperations operations;

public:
	Regular(int fd)
	: Node(&operations), _fd(fd) { }

private:
	int _fd;
};

const NodeOperations Regular::operations{
	&getRegularType,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	&Regular::open,
	nullptr,
	nullptr
};

struct Directory : Node {
private:
	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
			mkdir(std::shared_ptr<Node> object, std::string name), ([=] {
		(void)object;
		(void)name;
		assert(!"mkdir is not implemented for extern_fs");
	}))
	
	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
			symlink(std::shared_ptr<Node> object, std::string name, std::string link), ([=] {
		(void)object;
		(void)name;
		(void)link;
		assert(!"symlink is not implemented for extern_fs");
	}))
	
	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>, mkdev(std::shared_ptr<Node> object,
			std::string name, VfsType type, DeviceId id), ([=] {
		(void)object;
		(void)name;
		(void)type;
		(void)id;
		assert(!"mkdev is not implemented for extern_fs");
	}))

	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<Link>>,
			getLink(std::shared_ptr<Node> object, std::string name), ([=] {
		(void)object;
		int fd = open(name.c_str(), O_RDONLY);
		auto node = std::make_shared<Regular>(fd);
		// TODO: do not use createRootLink here!
		COFIBER_RETURN(createRootLink(std::move(node)));
	}))

	static const NodeOperations operations;

public:
	Directory()
	: Node(&operations) { }
};

const NodeOperations Directory::operations{
	&getDirectoryType,
	&Directory::getLink,
	&Directory::mkdir,
	&Directory::symlink,
	&Directory::mkdev,
	nullptr,
	nullptr,
	nullptr
};

} // anonymous namespace

std::shared_ptr<Link> createRoot() {
	auto node = std::make_shared<Directory>();
	return createRootLink(std::move(node));
}

} // namespace extern_fs

