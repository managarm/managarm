
#include "common.hpp"
#include "sysfile_fs.hpp"
#include "process.hpp"

namespace sysfile_fs {

// --------------------------------------------------------
// HelfdNode
// --------------------------------------------------------

void HelfdNode::openSelf(StdUnsafePtr<Process> process,
		frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) {
	auto open_file = frigg::makeShared<OpenFile>(*allocator, this);
	callback(frigg::staticPtrCast<VfsOpenFile>(frigg::move(open_file)));
}

// --------------------------------------------------------
// HelfdNode::OpenFile
// --------------------------------------------------------

HelfdNode::OpenFile::OpenFile(HelfdNode *inode)
: p_inode(inode) { }

void HelfdNode::OpenFile::setHelfd(HelHandle handle) {
	p_inode->p_handle = handle;
}

HelHandle HelfdNode::OpenFile::getHelfd() {
	return p_inode->p_handle;
}

// --------------------------------------------------------
// MountPoint
// --------------------------------------------------------

MountPoint::MountPoint() { }

void MountPoint::openMounted(StdUnsafePtr<Process> process,
		frigg::StringView path, uint32_t flags, uint32_t mode,
		frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) {
	size_t seperator = path.findFirst('/');
	assert(seperator == size_t(-1));
	assert((flags & MountSpace::kOpenCreat) != 0);
	
	StdSharedPtr<Inode> inode;
	if((mode & MountSpace::kOpenHelfd) != 0) {
		auto real_inode = frigg::makeShared<HelfdNode>(*allocator);
		inode = frigg::staticPtrCast<Inode>(frigg::move(real_inode));
	}else{
		assert(!"Mode not supported");
	}

	inode->openSelf(process, callback);
}

} // namespace sysfile_fs

