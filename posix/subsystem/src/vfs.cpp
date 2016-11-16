
#include <string.h>

#include <cofiber.hpp>
#include <cofiber/future.hpp>

#include "common.hpp"
#include "vfs.hpp"

#include <unistd.h>
#include <fcntl.h>

HelHandle __mlibc_getPassthrough(int fd);
HelHandle __raw_map(int fd);

namespace super_fs {

struct OpenFile {
	OpenFile(int fd)
	: _fd(fd) { }

	COFIBER_ROUTINE(vfs::FutureMaybe<off_t>, seek(off_t offset, VfsSeek whence), ([=] {
		assert(whence == VfsSeek::absolute);
		auto result = lseek(_fd, offset, SEEK_SET);
		assert(result != off_t(-1));
		COFIBER_RETURN(result);
	}))

	COFIBER_ROUTINE(vfs::FutureMaybe<size_t>, readSome(void *data, size_t max_length), ([=] {
		auto length = read(_fd, data, max_length);
		assert(length >= 0);
		COFIBER_RETURN(length);
	}))

	COFIBER_ROUTINE(vfs::FutureMaybe<helix::UniqueDescriptor>, accessMemory(), ([=] {
		COFIBER_RETURN(helix::UniqueDescriptor(__raw_map(_fd)));
	}))

	helix::BorrowedDescriptor getPassthroughLane() {
		return helix::BorrowedDescriptor(__mlibc_getPassthrough(_fd));
	}

private:
	int _fd;
};

struct Regular {
	Regular(int fd)
	: _fd(fd) { }

	COFIBER_ROUTINE(vfs::FutureMaybe<vfs::SharedFile>, open(vfs::SharedEntry entry), ([=] {
		COFIBER_RETURN(vfs::SharedFile::create<OpenFile>(std::move(entry), _fd));
	}))

private:
	int _fd;
};

struct Directory {
	COFIBER_ROUTINE(vfs::FutureMaybe<vfs::SharedNode>, resolveChild(std::string name), ([=] {
		int fd = open(name.c_str(), O_RDONLY);
		COFIBER_RETURN(vfs::SharedNode::createRegular<Regular>(fd));
	}))
};

} // namespace super_fs

namespace vfs {

// --------------------------------------------------------
// SharedFile implementation.
// --------------------------------------------------------

COFIBER_ROUTINE(FutureMaybe<void>, SharedFile::readExactly(void *data, size_t length) const,
		([=] {
	size_t offset = 0;
	while(offset < length) {
		auto result = COFIBER_AWAIT readSome((char *)data + offset,
				length - offset);
		assert(result > 0);
		offset += result;
	}

	COFIBER_RETURN();
}))

FutureMaybe<off_t> SharedFile::seek(off_t offset, VfsSeek whence) const {
	return _data->seek(offset, whence);
}

FutureMaybe<size_t> SharedFile::readSome(void *data, size_t max_length) const {
	return _data->readSome(data, max_length);
}

FutureMaybe<helix::UniqueDescriptor> SharedFile::accessMemory() const {
	return _data->accessMemory();
}

helix::BorrowedDescriptor SharedFile::getPassthroughLane() const {
	return _data->getPassthroughLane();
}

// --------------------------------------------------------
// SharedNode + SharedEntry implementation.
// --------------------------------------------------------

_node::EntryData::EntryData(SharedNode parent, std::string name,
		SharedNode target)
: parent(std::move(parent)), name(std::move(name)),
		target(std::move(target)) {
	if(parent._data) {
		assert(parent._data->getType() == Type::directory);
		auto parent_data = static_cast<DirectoryNodeData *>(parent._data.get());
		parent_data->dirElements.insert_equal(*this);
	}
}

SharedEntry _node::SharedEntry::attach(SharedNode parent, std::string name,
		SharedNode target) {
	auto data = std::make_shared<EntryData>(std::move(parent), std::move(name),
			std::move(target));
	return SharedEntry(std::move(data));
}

const std::string &_node::SharedEntry::getName() const {
	return _data->name;
}

SharedNode _node::SharedEntry::getTarget() const {
	return _data->target;
}

FutureMaybe<SharedFile> _node::SharedNode::open(SharedEntry entry) {
	assert(_data->getType() == Type::regular);
	auto data = static_cast<RegularNodeData *>(_data.get());
	return data->open(std::move(entry));
}

COFIBER_ROUTINE(FutureMaybe<SharedEntry>, _node::SharedNode::getChild(std::string name), ([=] {
	assert(_data->getType() == Type::directory);
	auto data = static_cast<DirectoryNodeData *>(_data.get());

	// TODO: use a logarithmic find function.
	for(auto it = data->dirElements.begin(); it != data->dirElements.end(); ++it)
		if(it->name == name)
			COFIBER_RETURN(SharedEntry(it->shared_from_this()));

	auto target = COFIBER_AWAIT data->resolveChild(name);
	COFIBER_RETURN(SharedEntry::attach(*this, std::move(name), std::move(target)));
}))

namespace {

SharedEntry createRootEntry() {
	auto node = SharedNode::createDirectory<super_fs::Directory>();
	return SharedEntry::attach(SharedNode(), std::string(), std::move(node));
}

} // anonymous namespace

SharedEntry rootEntry = createRootEntry();

COFIBER_ROUTINE(FutureMaybe<SharedFile>, open(std::string name), ([=] {
	auto entry = COFIBER_AWAIT rootEntry.getTarget().getChild(name);
	auto file = COFIBER_AWAIT entry.getTarget().open(entry);
	COFIBER_RETURN(std::move(file));
}))

} // namespace vfs

