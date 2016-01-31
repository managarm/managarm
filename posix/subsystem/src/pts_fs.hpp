
#ifndef POSIX_SUBSYSTEM_PTS_FS_HPP
#define POSIX_SUBSYSTEM_PTS_FS_HPP

#include <frigg/linked.hpp>

#include "vfs.hpp"

namespace pts_fs {

struct Endpoint {
	struct Chunk {
		Chunk();

		frigg::UniqueMemory<Allocator> buffer;
		size_t consumed;
	};
	
	struct ReadRequest {
		ReadRequest(void *buffer, size_t max_length,
				frigg::CallbackPtr<void(VfsError, size_t)> callback);
		
		void *buffer;
		size_t maxLength;
		frigg::CallbackPtr<void(VfsError, size_t)> callback;
	};

	Endpoint();

	void writeToQueue(const void *buffer, size_t length);
	void readFromQueue(void *buffer, size_t max_length,
			frigg::CallbackPtr<void(VfsError, size_t)> callback);

	frigg::LinkedList<Chunk, Allocator> chunkQueue;
	frigg::LinkedList<ReadRequest, Allocator> readQueue;
};

struct Terminal {
	Terminal(int number);

	Endpoint master, slave;

	int number;
};

struct Master : public VfsOpenFile {
	Master(frigg::SharedPtr<Terminal> terminal);

	// inherited from VfsOpenFile
	void write(const void *buffer, size_t length,
			frigg::CallbackPtr<void()> callback) override;
	void read(void *buffer, size_t max_length,
			frigg::CallbackPtr<void(VfsError, size_t)> callback) override;

	frigg::Optional<frigg::String<Allocator>> ttyName() override;

	const frigg::SharedPtr<Terminal> terminal;
};

struct Slave : public VfsOpenFile {
	Slave(frigg::SharedPtr<Terminal> terminal);

	// inherited from VfsOpenFile
	void write(const void *buffer, size_t length,
			frigg::CallbackPtr<void()> callback) override;
	void read(void *buffer, size_t max_length,
			frigg::CallbackPtr<void(VfsError, size_t)> callback) override;

	frigg::Optional<frigg::String<Allocator>> ttyName() override;

	const frigg::SharedPtr<Terminal> terminal;

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

private:
	frigg::Hashmap<int, frigg::WeakPtr<Terminal>,
			frigg::DefaultHasher<int>, Allocator> openTerminals;
	int nextTerminalNumber;
};

} // namespace pts_fs

#endif // POSIX_SUBSYSTEM_PTS_FS_HPP

