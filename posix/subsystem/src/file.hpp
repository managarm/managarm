#ifndef POSIX_SUBSYSTEM_FILE_HPP
#define POSIX_SUBSYSTEM_FILE_HPP

#include <async/result.hpp>
#include <boost/intrusive/rbtree.hpp>
#include <cofiber.hpp>
#include <cofiber/future.hpp>
#include <hel.h>

struct Node;

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
	File(std::shared_ptr<Node> node)
	: _node{std::move(node)} { }

	std::shared_ptr<Node> node() {
		return _node;
	}

	FutureMaybe<void> readExactly(void *data, size_t length);

	virtual FutureMaybe<off_t> seek(off_t offset, VfsSeek whence) = 0;
	virtual FutureMaybe<size_t> readSome(void *data, size_t max_length) = 0;
	virtual FutureMaybe<helix::UniqueDescriptor> accessMemory() = 0;
	virtual helix::BorrowedDescriptor getPassthroughLane() = 0;

private:
	const std::shared_ptr<Node> _node;
};

#endif // POSIX_SUBSYSTEM_FILE_HPP
