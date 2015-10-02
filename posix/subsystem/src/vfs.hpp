
#ifndef POSIX_SUBSYSTEM_VFS_HPP
#define POSIX_SUBSYSTEM_VFS_HPP

#include <frigg/string.hpp>
#include <frigg/hashmap.hpp>
#include <hel.h>

#include "device.hpp"

struct Process;

// --------------------------------------------------------
// VfsOpenFile
// --------------------------------------------------------

struct VfsOpenFile {
	virtual StdSharedPtr<VfsOpenFile> openAt(frigg::StringView path);
	
	virtual void write(const void *buffer, size_t length);
	virtual void read(void *buffer, size_t max_length, size_t &actual_length);

	virtual void setHelfd(HelHandle handle);
	virtual HelHandle getHelfd();
};

// --------------------------------------------------------
// VfsMountPoint
// --------------------------------------------------------

struct VfsMountPoint {
	virtual StdSharedPtr<VfsOpenFile> openMounted(StdUnsafePtr<Process> process,
			frigg::StringView path, uint32_t flags, uint32_t mode) = 0;
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

	StdSharedPtr<VfsOpenFile> openAbsolute(StdUnsafePtr<Process> process,
			frigg::StringView path, uint32_t flags, uint32_t mode);

	frigg::Hashmap<frigg::String<Allocator>, VfsMountPoint *,
			frigg::DefaultHasher<frigg::StringView>, Allocator> allMounts;

	DeviceAllocator charDevices;
	DeviceAllocator blockDevices;
};

#endif // POSIX_SUBSYSTEM_VFS_HPP

