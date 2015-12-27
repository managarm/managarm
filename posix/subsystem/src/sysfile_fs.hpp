
#ifndef POSIX_SUBSYSTEM_SYSFILE_FS_HPP
#define POSIX_SUBSYSTEM_SYSFILE_FS_HPP

#include <frigg/string.hpp>
#include <frigg/hashmap.hpp>

#include "vfs.hpp"

namespace sysfile_fs {

// --------------------------------------------------------
// Inode
// --------------------------------------------------------

struct Inode {
	virtual void openSelf(StdUnsafePtr<Process> process,
			frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) = 0;
};

// --------------------------------------------------------
// HelfdNode
// --------------------------------------------------------

class HelfdNode : public Inode {
public:
	class OpenFile : public VfsOpenFile {
	public:
		OpenFile(HelfdNode *inode);
		
		// inherited from VfsOpenFile
		void setHelfd(HelHandle handle) override;
		HelHandle getHelfd() override;
	
	private:
		HelfdNode *p_inode;
	};

	// inherited from Inode
	void openSelf(StdUnsafePtr<Process> process,
			frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) override;

private:
	HelHandle p_handle;
};

// --------------------------------------------------------
// MountPoint
// --------------------------------------------------------

class MountPoint : public VfsMountPoint {
public:
	MountPoint();
	
	// inherited from VfsMountPoint
	void openMounted(StdUnsafePtr<Process> process,
			frigg::String<Allocator> path, uint32_t flags, uint32_t mode,
			frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) override;
};

} // namespace sysfile_fs

#endif // POSIX_SUBSYSTEM_SYSFILE_FS_HPP

