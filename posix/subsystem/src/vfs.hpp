
#ifndef POSIX_SUBSYSTEM_VFS_HPP
#define POSIX_SUBSYSTEM_VFS_HPP

#include <frigg/string.hpp>
#include <frigg/hashmap.hpp>
#include <frigg/callback.hpp>
#include <hel.h>

#include "device.hpp"

struct Process;

struct FileStats {
	uint64_t fileSize;
};

// --------------------------------------------------------
// VfsOpenFile
// --------------------------------------------------------

struct VfsOpenFile {
	virtual void openAt(frigg::StringView path,
			frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback);
	
	virtual void fstat(frigg::CallbackPtr<void(FileStats)> callback);

	virtual void write(const void *buffer, size_t length,
			frigg::CallbackPtr<void()> callback);
	virtual void read(void *buffer, size_t max_length,
			frigg::CallbackPtr<void(size_t)> callback);

	virtual void setHelfd(HelHandle handle);
	virtual HelHandle getHelfd();
};

// --------------------------------------------------------
// VfsMountPoint
// --------------------------------------------------------

struct VfsMountPoint {
	virtual void openMounted(StdUnsafePtr<Process> process,
			frigg::StringView path, uint32_t flags, uint32_t mode,
			frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) = 0;
};

// --------------------------------------------------------
// MountSpace
// --------------------------------------------------------

struct MountSpace {
	enum OpenFlags : uint32_t {
		kOpenCreat = 1
	};

	enum OpenMode : uint32_t {
		kOpenHelfd = 1
	};

	MountSpace();

	void openAbsolute(StdUnsafePtr<Process> process,
			frigg::StringView path, uint32_t flags, uint32_t mode,
			frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback);

	frigg::Hashmap<frigg::String<Allocator>, VfsMountPoint *,
			frigg::DefaultHasher<frigg::StringView>, Allocator> allMounts;

	DeviceAllocator charDevices;
	DeviceAllocator blockDevices;
};

#endif // POSIX_SUBSYSTEM_VFS_HPP

