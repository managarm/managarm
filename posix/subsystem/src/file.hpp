#ifndef POSIX_SUBSYSTEM_FILE_HPP
#define POSIX_SUBSYSTEM_FILE_HPP

#include <async/result.hpp>
#include <boost/intrusive/rbtree.hpp>
#include <cofiber.hpp>
#include <cofiber/future.hpp>
#include <hel.h>

struct FsLink;

// TODO: Rename this enum as is not part of the VFS.
enum VfsError {
	success, eof
};

// TODO: Rename this enum as is not part of the VFS.
enum class VfsSeek {
	null, absolute, relative, eof
};

template<typename T>
using FutureMaybe = async::result<T>;

// ----------------------------------------------------------------------------
// File class.
// ----------------------------------------------------------------------------

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

	virtual FutureMaybe<off_t> seek(off_t offset, VfsSeek whence) = 0;
	virtual FutureMaybe<size_t> readSome(void *data, size_t max_length) = 0;
	virtual FutureMaybe<helix::UniqueDescriptor> accessMemory() = 0;
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
