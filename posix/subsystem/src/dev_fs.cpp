
#include "common.hpp"
#include "dev_fs.hpp"
#include "process.hpp"

namespace dev_fs {

// --------------------------------------------------------
// CharDeviceNode
// --------------------------------------------------------

CharDeviceNode::CharDeviceNode(unsigned int major, unsigned int minor)
: major(major), minor(minor) { }

void CharDeviceNode::openSelf(StdUnsafePtr<Process> process,
		frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) {
	StdUnsafePtr<Device> device = process->mountSpace->charDevices.getDevice(major, minor);
	assert(device);
	auto open_file = frigg::makeShared<OpenFile>(*allocator, device.toShared());
	callback(frigg::staticPtrCast<VfsOpenFile>(frigg::move(open_file)));
}

CharDeviceNode::OpenFile::OpenFile(StdSharedPtr<Device> device)
: p_device(frigg::move(device)) { }

void CharDeviceNode::OpenFile::write(const void *buffer, size_t length,
		frigg::CallbackPtr<void()> callback) {
	p_device->write(buffer, length);
	callback();
}

void CharDeviceNode::OpenFile::read(void *buffer, size_t max_length,
		frigg::CallbackPtr<void(VfsError, size_t)> callback) {
	size_t actual_length;
	p_device->read(buffer, max_length, actual_length);
	callback(kVfsSuccess, actual_length);
}

// --------------------------------------------------------
// DirectoryNode
// --------------------------------------------------------

DirectoryNode::DirectoryNode()
: entries(frigg::DefaultHasher<frigg::StringView>(), *allocator) { }

void DirectoryNode::openSelf(StdUnsafePtr<Process> process,
		frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) {
	assert(!"TODO: Implement this");
	__builtin_unreachable();
}

void DirectoryNode::openEntry(StdUnsafePtr<Process> process,
		frigg::String<Allocator> path, uint32_t flags, uint32_t mode,
		frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) {
	frigg::StringView segment;
	
	size_t seperator = frigg::StringView(path).findFirst('/');
	if(seperator == size_t(-1)) {
		auto entry = entries.get(path);
		if(entry) {
			(*entry)->openSelf(process, callback);
		}else if((flags & MountSpace::kOpenCreat) != 0) {
			StdSharedPtr<Inode> inode;
			//if((mode & MountSpace::kOpenHelfd) != 0) {
			//	auto real_inode = frigg::makeShared<HelfdNode>(*allocator);
			//	inode = frigg::staticPtrCast<Inode>(frigg::move(real_inode));
			//}else{
				assert(!"Mode not supported");
			//}

			entries.insert(frigg::String<Allocator>(*allocator, path),
					StdSharedPtr<Inode>(inode));
			inode->openSelf(process, callback);
		}else{
			callback(StdSharedPtr<VfsOpenFile>());
		}
	}else{
		assert(!"Not tested");
		frigg::StringView segment = frigg::StringView(path).subString(0, seperator);
		frigg::StringView tail = frigg::StringView(path).subString(seperator + 1,
				path.size() - (seperator + 1));
		
		auto entry = entries.get(segment);
		if(!entry) {
			callback(StdSharedPtr<VfsOpenFile>());
			return;
		}
		auto directory = frigg::staticPtrCast<DirectoryNode>(*entry);
		directory->openEntry(process, frigg::String<Allocator>(*allocator, tail),
				flags, mode, callback);
	}
}

// --------------------------------------------------------
// MountPoint
// --------------------------------------------------------

MountPoint::MountPoint() { }

void MountPoint::openMounted(StdUnsafePtr<Process> process,
		frigg::String<Allocator> path, uint32_t flags, uint32_t mode,
		frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) {
	rootDirectory.openEntry(process, frigg::move(path), flags, mode, callback);
}

} // namespace dev_fs
