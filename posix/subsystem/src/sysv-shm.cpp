#include <hel.h>

#include "sysv-shm.hpp"

namespace shm {

namespace {

// Global ID allocator for shmid values
int nextShmId = 1;

// Global registry of shared memory segments
std::map<int, std::shared_ptr<ShmSegment>> shmById;
std::map<key_t, std::shared_ptr<ShmSegment>> shmByKey;

void registerSegment(std::shared_ptr<ShmSegment> segment) {
	shmById[segment->shmid] = segment;
	if (segment->key != IPC_PRIVATE) {
		shmByKey[segment->key] = segment;
	}
}

int allocateShmId() {
	return nextShmId++;
}

std::shared_ptr<ShmSegment> findByKey(key_t key) {
	auto it = shmByKey.find(key);
	return it != shmByKey.end() ? it->second : nullptr;
}

std::shared_ptr<ShmSegment> createSegment(
		key_t key, size_t size, mode_t mode, pid_t cpid, uid_t uid, gid_t gid) {
	auto segment = std::make_shared<ShmSegment>();
	segment->shmid = allocateShmId();
	segment->key = key;
	segment->size = size;
	segment->mode = mode;
	segment->cpid = cpid;
	segment->ctime = 0;
	segment->uid = uid;
	segment->gid = gid;
	segment->cuid = uid;
	segment->cgid = gid;

	size_t alignedSize = (size + 0xFFF) & ~size_t(0xFFF);
	HelHandle handle;
	HEL_CHECK(helAllocateMemory(alignedSize, 0, nullptr, &handle));
	segment->memory = helix::UniqueDescriptor{handle};

	registerSegment(segment);
	return segment;
}

} // anonymous namespace

std::expected<std::shared_ptr<ShmSegment>, Error> createPrivateSegment(
		size_t size, mode_t mode, pid_t cpid, uid_t uid, gid_t gid) {
	if (size == 0)
		return std::unexpected{Error::illegalArguments};
	return createSegment(IPC_PRIVATE, size, mode, cpid, uid, gid);
}

std::expected<std::shared_ptr<ShmSegment>, Error> getOrCreateSegment(
		key_t key, size_t size, int flags, pid_t cpid, uid_t uid, gid_t gid) {
	auto segment = findByKey(key);
	if (segment) {
		if (flags & IPC_EXCL)
			return std::unexpected{Error::alreadyExists};
		if (size > segment->size)
			return std::unexpected{Error::illegalArguments};
		// TODO: Check permissions
		return segment;
	}

	// Segment doesn't exist
	if (!(flags & IPC_CREAT))
		return std::unexpected{Error::noSuchFile};
	if (size == 0)
		return std::unexpected{Error::illegalArguments};

	return createSegment(key, size, flags & 0777, cpid, uid, gid);
}

std::shared_ptr<ShmSegment> findById(int shmid) {
	auto it = shmById.find(shmid);
	return it != shmById.end() ? it->second : nullptr;
}

void removeSegment(std::shared_ptr<ShmSegment> segment) {
	shmById.erase(segment->shmid);
	if (segment->key != IPC_PRIVATE) {
		shmByKey.erase(segment->key);
	}
}

} // namespace shm
