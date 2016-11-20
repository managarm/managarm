
#include <string.h>
#include <future>

#include <cofiber.hpp>
#include <cofiber/future.hpp>

#include "common.hpp"
#include "vfs.hpp"
#include "extern_fs.hpp"

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

_vfs_node::EntryData::EntryData(SharedNode parent, std::string name,
		SharedNode target)
: parent(std::move(parent)), name(std::move(name)),
		target(std::move(target)) {
	if(parent._data) {
		assert(parent._data->getType() == Type::directory);
		auto parent_data = static_cast<DirectoryNodeData *>(parent._data.get());
		parent_data->dirElements.insert_equal(*this);
	}
}

SharedEntry _vfs_node::SharedEntry::attach(SharedNode parent, std::string name,
		SharedNode target) {
	auto data = std::make_shared<EntryData>(std::move(parent), std::move(name),
			std::move(target));
	return SharedEntry(std::move(data));
}

const std::string &_vfs_node::SharedEntry::getName() const {
	return _data->name;
}

SharedNode _vfs_node::SharedEntry::getTarget() const {
	return _data->target;
}

FutureMaybe<SharedFile> _vfs_node::SharedNode::open(SharedEntry entry) {
	assert(_data->getType() == Type::regular);
	auto data = static_cast<RegularNodeData *>(_data.get());
	return data->open(std::move(entry));
}

COFIBER_ROUTINE(FutureMaybe<SharedEntry>, _vfs_node::SharedNode::getChild(std::string name), ([=] {
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

COFIBER_ROUTINE(std::future<SharedEntry>, createRootEntry(), ([=] {
	auto node = extern_fs::createRootNode();
	COFIBER_RETURN(SharedEntry::attach(SharedNode(), std::string(), std::move(node)));
}))

} // anonymous namespace

SharedEntry rootEntry = createRootEntry().get();

COFIBER_ROUTINE(FutureMaybe<SharedFile>, open(std::string name), ([=] {
	auto entry = COFIBER_AWAIT rootEntry.getTarget().getChild(name);
	auto file = COFIBER_AWAIT entry.getTarget().open(entry);
	COFIBER_RETURN(std::move(file));
}))

