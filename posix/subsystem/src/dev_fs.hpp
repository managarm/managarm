
#ifndef POSIX_SUBSYSTEM_DEV_FS_HPP
#define POSIX_SUBSYSTEM_DEV_FS_HPP

#include <frigg/string.hpp>
#include <frigg/hashmap.hpp>

#include "vfs.hpp"

namespace dev_fs {

// --------------------------------------------------------
// Inode
// --------------------------------------------------------

struct Inode {
	virtual void openSelf(StdUnsafePtr<Process> process,
			frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) = 0;
};

// --------------------------------------------------------
// CharDeviceNode
// --------------------------------------------------------

class CharDeviceNode : public Inode {
public:
	class OpenFile : public VfsOpenFile {
	public:
		OpenFile(StdSharedPtr<Device> device);
		
		// inherited from VfsOpenFile
		void write(const void *buffer, size_t length,
				frigg::CallbackPtr<void()> callback) override;
		void read(void *buffer, size_t max_length,
				frigg::CallbackPtr<void(VfsError, size_t)> callback) override;
	
	private:
		StdSharedPtr<Device> p_device;
	};

	CharDeviceNode(unsigned int major, unsigned int minor);

	// inherited from Inode
	void openSelf(StdUnsafePtr<Process> process,
			frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) override;

private:
	unsigned int major, minor;
};

// --------------------------------------------------------
// DirectoryNode
// --------------------------------------------------------

struct DirectoryNode : public Inode {
	DirectoryNode();

	void openEntry(StdUnsafePtr<Process> process,
			frigg::StringView path, uint32_t flags, uint32_t mode,
			frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback);
	
	// inherited from Inode
	void openSelf(StdUnsafePtr<Process> process,
			frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) override;

	frigg::Hashmap<frigg::String<Allocator>, StdSharedPtr<Inode>,
			frigg::DefaultHasher<frigg::StringView>, Allocator> entries;
};

// --------------------------------------------------------
// MountPoint
// --------------------------------------------------------

class MountPoint : public VfsMountPoint {
public:
	MountPoint();
	
	DirectoryNode *getRootDirectory() {
		return &rootDirectory;
	}
	
	// inherited from VfsMountPoint
	void openMounted(StdUnsafePtr<Process> process,
			frigg::StringView path, uint32_t flags, uint32_t mode,
			frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) override;
	
private:
	DirectoryNode rootDirectory;
};

} // namespace dev_fs

#endif // POSIX_SUBSYSTEM_DEV_FS_HPP

