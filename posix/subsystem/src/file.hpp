#ifndef POSIX_SUBSYSTEM_FILE_HPP
#define POSIX_SUBSYSTEM_FILE_HPP

#include <variant>

#include <async/result.hpp>
#include <boost/intrusive/rbtree.hpp>
#include <cofiber.hpp>
#include <cofiber/future.hpp>
#include <hel.h>

struct File;
struct FsLink;

// TODO: Rename this enum as is not part of the VFS.
enum class Error {
	success,
	eof,

	// Indices that the given object does not support the operation
	// (e.g. readSymlink() is called on a file that is not a link).
	illegalOperationTarget
};

// TODO: Rename this enum as is not part of the VFS.
enum class VfsSeek {
	null, absolute, relative, eof
};

template<typename T>
using FutureMaybe = async::result<T>;

template<typename T>
using expected = async::result<std::variant<Error, T>>;

// ----------------------------------------------------------------------------
// File class.
// ----------------------------------------------------------------------------

using PollResult = std::tuple<uint64_t, int, int>;

using RecvResult = std::pair<size_t, std::vector<std::shared_ptr<File>>>;

struct File {
	File(std::shared_ptr<FsLink> link)
	: _link{std::move(link)} { }

	// This is the link that was used to open the file.
	// Note that this might not be the only link that can be used
	// to reach the file's inode.
	std::shared_ptr<FsLink> associatedLink() {
		return _link;
	}

	FutureMaybe<void> readExactly(void *data, size_t length);

	virtual FutureMaybe<off_t> seek(off_t offset, VfsSeek whence);
	virtual FutureMaybe<size_t> readSome(void *data, size_t max_length) = 0;
	
	virtual FutureMaybe<size_t> sendMsg(const void *data, size_t max_length,
			std::vector<std::shared_ptr<File>> files);
	
	virtual FutureMaybe<RecvResult> recvMsg(void *data, size_t max_length);

	// poll() uses a sequence number mechansim for synchronization.
	// Before returning, it waits until current-sequence > in-sequence.
	// Returns (current-sequence, edges since in-sequence, current events).
	// current-sequence is incremented each time an edge (i.e. an event bit
	// transitions from clear to set) happens.
	// TODO: This request should be cancelable.
	virtual FutureMaybe<PollResult> poll(uint64_t sequence);

	// TODO: This should not depend on an offset.
	// Due to missing support from the kernel, we currently need multiple memory
	// objects per file for DRM device files.
	virtual FutureMaybe<helix::UniqueDescriptor> accessMemory(off_t offset = 0);

	virtual helix::BorrowedDescriptor getPassthroughLane() = 0;

private:
	const std::shared_ptr<FsLink> _link;
};

// This class represents files that are part of an actual file system.
// Their operations are provided by that file system.
struct ProperFile : File {
	ProperFile(std::shared_ptr<FsLink> link)
	: File{std::move(link)} { }
};

// Represents files that have a link in a file system but that
// have operations that are provided externally.
// This concerns mainly devices and UNIX sockets.
struct ProxyFile : File {
	ProxyFile(std::shared_ptr<FsLink> link)
	: File{std::move(link)} { }
};

#endif // POSIX_SUBSYSTEM_FILE_HPP
