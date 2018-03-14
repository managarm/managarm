#ifndef LIBFS_SERVER_HPP
#define LIBFS_SERVER_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <async/result.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/common.hpp>
#include <smarter.hpp>

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

using AccessMemoryResult = std::pair<helix::BorrowedDescriptor, uint64_t>;

using GetLinkResult = std::tuple<std::shared_ptr<void>, int64_t, FileType>;

using OpenResult = std::pair<helix::UniqueLane, helix::UniqueLane>;

struct FileOperations {
	constexpr FileOperations()
	: seekAbs{nullptr}, seekRel{nullptr}, seekEof{nullptr},
			read{nullptr}, write{nullptr}, readEntries{nullptr},
			accessMemory{nullptr}, truncate{nullptr}, fallocate{nullptr},
			ioctl{nullptr}, poll{nullptr},
			bind{nullptr}, listen{nullptr}, connect{nullptr} { }

	constexpr FileOperations &withSeekAbs(async::result<int64_t> (*f)(void *object,
			int64_t offset)) {
		seekAbs = f;
		return *this;
	}
	constexpr FileOperations &withSeekRel(async::result<int64_t> (*f)(void *object,
			int64_t offset)) {
		seekRel = f;
		return *this;
	}
	constexpr FileOperations &withSeekEof(async::result<int64_t> (*f)(void *object,
			int64_t offset)) {
		seekEof = f;
		return *this;
	}
	constexpr FileOperations &withRead(async::result<ReadResult> (*f)(void *object,
			void *buffer, size_t length)) {
		read = f;
		return *this;
	}
	constexpr FileOperations &withWrite(async::result<void> (*f)(void *object,
			const void *buffer, size_t length)) {
		write = f;
		return *this;
	}
	constexpr FileOperations &withReadEntries(async::result<ReadEntriesResult> (*f)(void *object)) {
		readEntries = f;
		return *this;
	}
	constexpr FileOperations &withAccessMemory(async::result<AccessMemoryResult>(*f)(void *object,
			uint64_t offset, size_t size)) {
		accessMemory = f;
		return *this;
	}
	constexpr FileOperations &withTruncate(async::result<void> (*f)(void *object,
			size_t size)) {
		truncate = f;
		return *this;
	}
	constexpr FileOperations &withFallocate(async::result<void> (*f)(void *object,
			int64_t offset, size_t size)) {
		fallocate = f;
		return *this;
	}
	constexpr FileOperations &withIoctl(async::result<void> (*f)(void *object,
			managarm::fs::CntRequest req, helix::UniqueLane conversation)) {
		ioctl = f;
		return *this;
	}
	constexpr FileOperations &withPoll(async::result<PollResult> (*f)(void *object,
			uint64_t sequence)) {
		poll = f;
		return *this;
	}
	constexpr FileOperations &withBind(async::result<void> (*f)(void *object,
			const void *addr_ptr, size_t addr_length)) {
		bind = f;
		return *this;
	}
	constexpr FileOperations &withConnect(async::result<void> (*f)(void *object,
			const void *addr_ptr, size_t addr_length)) {
		connect = f;
		return *this;
	}

	async::result<int64_t> (*seekAbs)(void *object, int64_t offset);
	async::result<int64_t> (*seekRel)(void *object, int64_t offset);
	async::result<int64_t> (*seekEof)(void *object, int64_t offset);
	async::result<ReadResult> (*read)(void *object, void *buffer, size_t length);
	async::result<void> (*write)(void *object, const void *buffer, size_t length);
	async::result<ReadEntriesResult> (*readEntries)(void *object);
	async::result<AccessMemoryResult>(*accessMemory)(void *object,
			uint64_t offset, size_t size);
	async::result<void> (*truncate)(void *object, size_t size);
	async::result<void> (*fallocate)(void *object, int64_t offset, size_t size);
	async::result<void> (*ioctl)(void *object, managarm::fs::CntRequest req,
			helix::UniqueLane conversation);
	async::result<PollResult> (*poll)(void *object, uint64_t sequence);
	async::result<void> (*bind)(void *object, const void *addr_ptr, size_t addr_length);
	async::result<void> (*listen)(void *object);
	async::result<void> (*connect)(void *object, const void *addr_ptr, size_t addr_length);
};

struct NodeOperations {
	async::result<GetLinkResult> (*getLink)(std::shared_ptr<void> object,
			std::string name);

	async::result<OpenResult> (*open)(std::shared_ptr<void> object);

	async::result<std::string> (*readSymlink)(std::shared_ptr<void> object);
};

async::result<void>
serveFile(helix::UniqueLane lane, void *file, const FileOperations *file_ops);

async::cancelable_result<void>
servePassthrough(helix::UniqueLane lane, smarter::shared_ptr<void> file,
		const FileOperations *file_ops);

cofiber::no_future serveNode(helix::UniqueLane lane, std::shared_ptr<void> node,
		const NodeOperations *node_ops);


} } // namespace protocols::fs

#endif // LIBFS_SERVER_HPP
