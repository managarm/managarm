
#include "common.hpp"
#include "dev_fs.hpp"
#include "process.hpp"

namespace dev_fs {

// --------------------------------------------------------
// CharDeviceNode
// --------------------------------------------------------

CharDeviceNode::CharDeviceNode(unsigned int major, unsigned int minor)
: major(major), minor(minor) { }

StdSharedPtr<VfsOpenFile> CharDeviceNode::openSelf(StdUnsafePtr<Process> process) {
	StdUnsafePtr<Device> device = process->mountSpace->charDevices.getDevice(major, minor);
	assert(device);
	auto open_file = frigg::makeShared<OpenFile>(*allocator, StdSharedPtr<Device>(device));
	return frigg::staticPointerCast<VfsOpenFile>(frigg::move(open_file));
}

CharDeviceNode::OpenFile::OpenFile(StdSharedPtr<Device> device)
: p_device(frigg::move(device)) { }

void CharDeviceNode::OpenFile::write(const void *buffer, size_t length) {
	p_device->write(buffer, length);
}

void CharDeviceNode::OpenFile::read(void *buffer, size_t max_length, size_t &actual_length) {
	p_device->read(buffer, max_length, actual_length);
}

// --------------------------------------------------------
// CharDeviceNode
// --------------------------------------------------------

StdSharedPtr<VfsOpenFile> HelfdNode::openSelf(StdUnsafePtr<Process> process) {
	auto open_file = frigg::makeShared<OpenFile>(*allocator, this);
	return frigg::staticPointerCast<VfsOpenFile>(frigg::move(open_file));
}

HelfdNode::OpenFile::OpenFile(HelfdNode *inode)
: p_inode(inode) { }

void HelfdNode::OpenFile::setHelfd(HelHandle handle) {
	p_inode->p_handle = handle;
}
HelHandle HelfdNode::OpenFile::getHelfd() {
	return p_inode->p_handle;
}

// --------------------------------------------------------
// CharDeviceNode
// --------------------------------------------------------

DirectoryNode::DirectoryNode()
: entries(frigg::DefaultHasher<frigg::StringView>(), *allocator) { }

StdSharedPtr<VfsOpenFile> DirectoryNode::openSelf(StdUnsafePtr<Process> process) {
	assert(!"TODO: Implement this");
	__builtin_unreachable();
}

StdSharedPtr<VfsOpenFile> DirectoryNode::openRelative(StdUnsafePtr<Process> process,
		frigg::StringView path, uint32_t flags, uint32_t mode) {
	frigg::StringView segment;
	
	size_t seperator = path.findFirst('/');
	if(seperator == size_t(-1)) {
		auto entry = entries.get(path);
		if(entry) {
			return (**entry)->openSelf(process);
		}else if((flags & MountSpace::kOpenCreat) != 0) {
			StdSharedPtr<Inode> inode;
			if((mode & MountSpace::kOpenHelfd) != 0) {
				auto real_inode = frigg::makeShared<HelfdNode>(*allocator);
				inode = frigg::staticPointerCast<Inode>(frigg::move(real_inode));
			}else{
				assert(!"mode not supported");
			}
			StdSharedPtr<VfsOpenFile> open_file = inode->openSelf(process);
			entries.insert(frigg::String<Allocator>(*allocator, path), frigg::move(inode));
			return open_file;
		}else{
			return StdSharedPtr<VfsOpenFile>();
		}
	}else{
		assert(!"Not tested");
		frigg::StringView segment = path.subString(0, seperator);
		frigg::StringView tail = path.subString(seperator + 1, path.size() - (seperator + 1));
		
		auto entry = entries.get(segment);
		if(!entry)
			return StdSharedPtr<VfsOpenFile>();
		auto directory = frigg::staticPointerCast<DirectoryNode>(**entry);
		return directory->openRelative(process, tail, flags, mode);
	}
}

// --------------------------------------------------------
// CharDeviceNode
// --------------------------------------------------------

MountPoint::MountPoint() { }

StdSharedPtr<VfsOpenFile> MountPoint::openMounted(StdUnsafePtr<Process> process,
		frigg::StringView path, uint32_t flags, uint32_t mode) {
	return rootDirectory.openRelative(process, path, flags, mode);
}

} // namespace dev_fs
