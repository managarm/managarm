#pragma once

#include <sys/ipc.h>
#include <sys/shm.h>
#include <expected>
#include <map>
#include <memory>

#include <helix/memory.hpp>

#include "file.hpp"

namespace shm {

// Represents a SysV shared memory segment
struct ShmSegment : std::enable_shared_from_this<ShmSegment> {
	int shmid;
	key_t key;
	size_t size;

	// Backing memory for the shared memory segment
	helix::UniqueDescriptor memory;

	// Permission info
	uid_t uid = 0;
	gid_t gid = 0;
	uid_t cuid = 0;
	gid_t cgid = 0;
	mode_t mode = 0;
	int seq = 0;

	// Process tracking
	pid_t cpid = 0;  // Creator PID
	pid_t lpid = 0;  // Last attach/detach PID
	size_t nattch = 0;  // Number of current attaches

	// Timestamps
	time_t atime = 0;  // Last attach time
	time_t dtime = 0;  // Last detach time
	time_t ctime = 0;  // Creation/change time

	// If true, segment will be destroyed when nattch reaches 0
	bool markedForRemoval = false;
};

// Segment management functions
std::expected<std::shared_ptr<ShmSegment>, Error> createPrivateSegment(
		size_t size, mode_t mode, pid_t cpid, uid_t uid, gid_t gid);
std::expected<std::shared_ptr<ShmSegment>, Error> getOrCreateSegment(
		key_t key, size_t size, int flags, pid_t cpid, uid_t uid, gid_t gid);
std::shared_ptr<ShmSegment> findById(int shmid);
void removeSegment(std::shared_ptr<ShmSegment> segment);

} // namespace shm
