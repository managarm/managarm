
#ifndef POSIX_SUBSYSTEM_EXTERN_FS_HPP
#define POSIX_SUBSYSTEM_EXTERN_FS_HPP

#include "vfs.hpp"

namespace extern_fs {

struct MountPoint;

struct OpenFile : public VfsOpenFile {
	OpenFile(MountPoint &connection, int extern_fd);

	// inherited from VfsOpenFile
	void connect(frigg::CallbackPtr<void()> callback) override;
	void fstat(frigg::CallbackPtr<void(FileStats)> callback) override;
	void write(const void *buffer, size_t length,
			frigg::CallbackPtr<void()> callback) override;
	void read(void *buffer, size_t max_length,
			frigg::CallbackPtr<void(VfsError, size_t)> callback) override;

	void seek(int64_t rel_offset, VfsSeek whence,
			frigg::CallbackPtr<void(uint64_t)> callback) override;

	void mmap(frigg::CallbackPtr<void(HelHandle)> callback) override;
	
	MountPoint &connection;
	int externFd;
};

// --------------------------------------------------------
// MountPoint
// --------------------------------------------------------

class MountPoint : public VfsMountPoint {
public:
	MountPoint(helx::Pipe pipe);
	
	// inherited from VfsMountPoint
	void openMounted(StdUnsafePtr<Process> process,
			frigg::String<Allocator> path, uint32_t flags, uint32_t mode,
			frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) override;

	helx::Pipe &getPipe();

private:
	helx::Pipe p_pipe;
};

// --------------------------------------------------------
// Closures
// --------------------------------------------------------

struct OpenClosure {
	OpenClosure(MountPoint &connection, frigg::String<Allocator> path,
			frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback);

	void operator() ();

private:
	void recvOpenResponse(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);
	void recvReadResponse(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);
	void recvReadData(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	MountPoint &connection;
	frigg::String<Allocator> path;
	frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback;

	uint8_t buffer[128];
	int externFd;
	char dataBuffer[128];
	frigg::String<Allocator> linkTarget;
};

} // namespace extern_fs

#endif // POSIX_SUBSYSTEM_EXTERN_FS_HPP

