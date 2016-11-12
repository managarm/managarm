
#ifndef POSIX_SUBSYSTEM_VFS_HPP
#define POSIX_SUBSYSTEM_VFS_HPP

#include <hel.h>

//#include "device.hpp"

struct Process;

enum VfsError {
	success, eof
};

enum class VfsSeek {
	null, absolute, relative, eof
};

struct FileStats {
	uint64_t inodeNumber;
	uint32_t mode;
	int numLinks;
	int uid, gid;
	uint64_t fileSize;
	uint64_t atimeSecs, atimeNanos;
	uint64_t mtimeSecs, mtimeNanos;
	uint64_t ctimeSecs, ctimeNanos;
};

/*frigg::String<Allocator> normalizePath(frigg::StringView path);

frigg::String<Allocator> concatenatePath(frigg::StringView prefix,
		frigg::StringView path);*/

// --------------------------------------------------------
// VfsOpenFile
// --------------------------------------------------------

struct VfsOpenFile {
	cofiber::future<void> readExactly(void *buffer, size_t length);

//	virtual void openAt(std::string path,
//			frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback);

//	virtual void connect(frigg::CallbackPtr<void()> callback);

//	virtual void fstat(frigg::CallbackPtr<void(FileStats)> callback);

//	virtual void write(const void *buffer, size_t length,
//			frigg::CallbackPtr<void()> callback);
	virtual cofiber::future<std::tuple<VfsError, size_t>> readSome(void *buffer,
			size_t max_length);
	
	virtual cofiber::future<uint64_t> seek(int64_t offset, VfsSeek whence);
	
	virtual cofiber::future<helix::UniqueDescriptor> accessMemory();

//	virtual cofiber::future<std::string> ttyName();
};
/*
// --------------------------------------------------------
// VfsMountPoint
// --------------------------------------------------------

struct VfsMountPoint {
	virtual void openMounted(StdUnsafePtr<Process> process,
			frigg::String<Allocator> path, uint32_t flags, uint32_t mode,
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
			frigg::String<Allocator> path, uint32_t flags, uint32_t mode,
			frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback);

	frigg::Hashmap<frigg::String<Allocator>, VfsMountPoint *,
			frigg::DefaultHasher<frigg::StringView>, Allocator> allMounts;

	DeviceAllocator charDevices;
	DeviceAllocator blockDevices;
};*/

#endif // POSIX_SUBSYSTEM_VFS_HPP

