
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

struct StatClosure {
	StatClosure(MountPoint &connection, int extern_fd,
			frigg::CallbackPtr<void(FileStats)> callback);

	void operator() ();

private:
	void recvResponse(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	MountPoint &connection;
	int externFd;
	frigg::CallbackPtr<void(FileStats)> callback;
	uint8_t buffer[128];
};

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

struct ConnectClosure {
	ConnectClosure(MountPoint &connection, int extern_fd,
			frigg::CallbackPtr<void()> callback);

	void operator() ();

private:
	void recvResponse(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	MountPoint &connection;
	int externFd;
	frigg::CallbackPtr<void()> callback;
	uint8_t buffer[128];
};

struct ReadClosure {
	ReadClosure(MountPoint &connection, int extern_fd, void *read_buffer, size_t max_size,
			frigg::CallbackPtr<void(VfsError, size_t)> callback);

	void operator() ();

private:
	void recvResponse(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);
	void recvData(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	MountPoint &connection;
	int externFd;
	void *readBuffer;
	size_t maxSize;
	frigg::CallbackPtr<void(VfsError, size_t)> callback;
	uint8_t buffer[128];
};

struct WriteClosure {
	WriteClosure(MountPoint &connection, int extern_fd, const void *write_buffer, size_t size,
			frigg::CallbackPtr<void()> callback);

	void operator() ();

private:
	void recvResponse(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	MountPoint &connection;
	int externFd;
	const void *writeBuffer;
	size_t size;
	frigg::CallbackPtr<void()> callback;
	uint8_t buffer[128];
};

struct SeekClosure {
	SeekClosure(MountPoint &connection, int extern_fd, int64_t rel_offset, VfsSeek whence,
			frigg::CallbackPtr<void(uint64_t)> callback);

	void operator() ();

private:
	void recvResponse(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	MountPoint &connection;
	int externFd;
	int64_t relOffset;
	VfsSeek whence;
	frigg::CallbackPtr<void(uint64_t)> callback;
	uint8_t buffer[128];
};

struct MapClosure {
	MapClosure(MountPoint &connection, int extern_fd,
			frigg::CallbackPtr<void(HelHandle)> callback);

	void operator() ();

private:
	void recvResponse(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);
	void recvHandle(HelError error, int64_t msg_request, int64_t msg_seq, HelHandle file_memory);

	MountPoint &connection;
	int externFd;
	frigg::CallbackPtr<void(HelHandle)> callback;
	uint8_t buffer[128];
};


} // namespace extern_fs

#endif // POSIX_SUBSYSTEM_EXTERN_FS_HPP

