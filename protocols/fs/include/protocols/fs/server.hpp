#ifndef LIBFS_SERVER_HPP
#define LIBFS_SERVER_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <async/result.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>

namespace managarm::fs {
	struct CntRequest;
}

namespace protocols {
namespace fs {

enum class FileType {
	unknown,
	directory,
	regular,
	symlink
};

using GetLinkResult = std::tuple<std::shared_ptr<void>, int64_t, FileType>;

using AccessMemoryResult = std::pair<helix::BorrowedDescriptor, uint64_t>;

struct FileOperations {
	constexpr FileOperations()
	: seekAbs{nullptr}, seekRel{nullptr}, seekEof{nullptr},
			read{nullptr}, write{nullptr}, accessMemory{nullptr}, ioctl{nullptr} { }

	constexpr FileOperations &withSeekAbs(async::result<int64_t> (*f)(std::shared_ptr<void> object, int64_t offset)) {
		seekAbs = f;
		return *this;
	}
	constexpr FileOperations &withSeekRel(async::result<int64_t> (*f)(std::shared_ptr<void> object, int64_t offset)) {
		seekRel = f;
		return *this;
	}
	constexpr FileOperations &withSeekEof(async::result<int64_t> (*f)(std::shared_ptr<void> object, int64_t offset)) {
		seekEof = f;
		return *this;
	}
	constexpr FileOperations &withRead(async::result<size_t> (*f)(std::shared_ptr<void> object, void *buffer, size_t length)) {
		read = f;
		return *this;
	}
	constexpr FileOperations &withWrite(async::result<void> (*f)(std::shared_ptr<void> object, const void *buffer, size_t length)) {
		write = f;
		return *this;
	}
	constexpr FileOperations &withAccessMemory(async::result<AccessMemoryResult>(*f)(std::shared_ptr<void> object,
			uint64_t offset, size_t size)) {
		accessMemory = f;
		return *this;
	}
	constexpr FileOperations &withIoctl(async::result<void> (*f)(std::shared_ptr<void> object, managarm::fs::CntRequest req,
			helix::UniqueLane conversation)) {
		ioctl = f;
		return *this;
	}

	async::result<int64_t> (*seekAbs)(std::shared_ptr<void> object, int64_t offset);
	async::result<int64_t> (*seekRel)(std::shared_ptr<void> object, int64_t offset);
	async::result<int64_t> (*seekEof)(std::shared_ptr<void> object, int64_t offset);
	async::result<size_t> (*read)(std::shared_ptr<void> object, void *buffer, size_t length);
	async::result<void> (*write)(std::shared_ptr<void> object, const void *buffer, size_t length);
	async::result<AccessMemoryResult>(*accessMemory)(std::shared_ptr<void> object,
			uint64_t offset, size_t size);
	async::result<void> (*ioctl)(std::shared_ptr<void> object, managarm::fs::CntRequest req,
			helix::UniqueLane conversation);
};

struct NodeOperations {
	async::result<GetLinkResult> (*getLink)(std::shared_ptr<void> object,
			std::string name);

	async::result<std::shared_ptr<void>> (*open)(std::shared_ptr<void> object);

	async::result<std::string> (*readSymlink)(std::shared_ptr<void> object);
};

cofiber::no_future servePassthrough(helix::UniqueLane lane, std::shared_ptr<void> node,
		const FileOperations *file_ops);

cofiber::no_future serveNode(helix::UniqueLane lane, std::shared_ptr<void> node,
		const NodeOperations *node_ops, const FileOperations *file_ops);


} } // namespace protocols::fs

#endif // LIBFS_SERVER_HPP
