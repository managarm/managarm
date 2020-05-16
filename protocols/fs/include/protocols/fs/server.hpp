#ifndef LIBFS_SERVER_HPP
#define LIBFS_SERVER_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <async/cancellation.hpp>
#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>
#include <protocols/fs/common.hpp>
#include <protocols/fs/defs.hpp>
#include <smarter.hpp>

namespace managarm::fs {
	class CntRequest;
}

namespace protocols {
namespace fs {

enum class FileType {
	unknown,
	directory,
	regular,
	symlink
};

struct FileStats {
	int linkCount;
	uint64_t fileSize;
	uint32_t mode;
	int uid, gid;
	struct timespec accessTime;
	struct timespec dataModifyTime;
	struct timespec anyChangeTime;
};

using SeekResult = std::variant<Error, int64_t>;

using GetLinkResult = std::tuple<std::shared_ptr<void>, int64_t, FileType>;

using OpenResult = std::pair<helix::UniqueLane, helix::UniqueLane>;

using MkdirResult = std::pair<std::shared_ptr<void>, int64_t>;

struct FileOperations {
	constexpr FileOperations &withSeekAbs(async::result<SeekResult> (*f)(void *object,
			int64_t offset)) {
		seekAbs = f;
		return *this;
	}
	constexpr FileOperations &withSeekRel(async::result<SeekResult> (*f)(void *object,
			int64_t offset)) {
		seekRel = f;
		return *this;
	}
	constexpr FileOperations &withSeekEof(async::result<SeekResult> (*f)(void *object,
			int64_t offset)) {
		seekEof = f;
		return *this;
	}
	constexpr FileOperations &withRead(async::result<ReadResult> (*f)(void *object,
			const char *, void *buffer, size_t length)) {
		read = f;
		return *this;
	}
	constexpr FileOperations &withWrite(async::result<void> (*f)(void *object,
			const char *, const void *buffer, size_t length)) {
		write = f;
		return *this;
	}
	constexpr FileOperations &withReadEntries(async::result<ReadEntriesResult> (*f)(void *object)) {
		readEntries = f;
		return *this;
	}
	constexpr FileOperations &withAccessMemory(async::result<helix::BorrowedDescriptor>(*f)(void *object)) {
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
	constexpr FileOperations &withFlock(async::result<protocols::fs::Error> (*f)(void *object,
			int flags)) {
		flock = f;
		return *this;
	}
	constexpr FileOperations &withGetOption(async::result<int> (*f)(void *object,
			int option)) {
		getOption = f;
		return *this;
	}
	constexpr FileOperations &withSetOption(async::result<void> (*f)(void *object,
			int option, int value)) {
		setOption = f;
		return *this;
	}
	constexpr FileOperations &withPoll(async::result<PollResult> (*f)(void *object,
			uint64_t sequence, async::cancellation_token cancellation)) {
		poll = f;
		return *this;
	}
	constexpr FileOperations &withBind(async::result<Error> (*f)(void *object,
			const char *, const void *addr_ptr, size_t addr_length)) {
		bind = f;
		return *this;
	}
	constexpr FileOperations &withConnect(async::result<Error> (*f)(void *object,
			const char *, const void *addr_ptr, size_t addr_length)) {
		connect = f;
		return *this;
	}
	constexpr FileOperations &withSockname(async::result<size_t> (*f)(void *object,
			void *addr_ptr, size_t max_addr_length)) {
		sockname = f;
		return *this;
	}

	async::result<SeekResult> (*seekAbs)(void *object, int64_t offset);
	async::result<SeekResult> (*seekRel)(void *object, int64_t offset);
	async::result<SeekResult> (*seekEof)(void *object, int64_t offset);
	async::result<ReadResult> (*read)(void *object, const char *credentials,
			void *buffer, size_t length);
	async::result<ReadResult> (*pread)(void *object, int64_t offset, const char *credentials,
			void *buffer, size_t length);
	async::result<void> (*write)(void *object, const char *credentials,
			const void *buffer, size_t length);
	async::result<ReadEntriesResult> (*readEntries)(void *object);
	async::result<helix::BorrowedDescriptor>(*accessMemory)(void *object);
	async::result<void> (*truncate)(void *object, size_t size);
	async::result<void> (*fallocate)(void *object, int64_t offset, size_t size);
	async::result<void> (*ioctl)(void *object, managarm::fs::CntRequest req,
			helix::UniqueLane conversation);
	async::result<protocols::fs::Error> (*flock)(void *object, int flags);
	async::result<int> (*getOption)(void *object, int option);
	async::result<void> (*setOption)(void *object, int option, int value);
	async::result<PollResult> (*poll)(void *object, uint64_t sequence,
			async::cancellation_token cancellation);
	async::result<Error> (*bind)(void *object, const char *credentials,
			const void *addr_ptr, size_t addr_length);
	async::result<void> (*listen)(void *object);
	async::result<Error> (*connect)(void *object, const char *credentials,
			const void *addr_ptr, size_t addr_length);
	async::result<size_t> (*sockname)(void *object, void *addr_ptr, size_t max_addr_length);
	async::result<int> (*getFileFlags)(void *object);
	async::result<void> (*setFileFlags)(void *object, int flags);
	async::result<RecvResult> (*recvMsg)(void *object, const char *creds,
			uint32_t flags, void *data, size_t len,
			void *addr_buf, size_t addr_size, size_t max_ctrl_len);
	async::result<SendResult> (*sendMsg)(void *object, const char *creds,
			uint32_t flags, void *data, size_t len,
			void *addr_buf, size_t addr_size,
			std::vector<uint32_t> fds);
};

struct StatusPageProvider {
	StatusPageProvider();

	helix::BorrowedDescriptor getMemory() {
		return _memory;
	}

	void update(uint64_t sequence, int status);

private:
	helix::UniqueDescriptor _memory;
	helix::Mapping _mapping;
};

struct NodeOperations {
	async::result<FileStats> (*getStats)(std::shared_ptr<void> object);

	async::result<GetLinkResult> (*getLink)(std::shared_ptr<void> object,
			std::string name);

	async::result<GetLinkResult> (*link)(std::shared_ptr<void> object,
			std::string name, int64_t ino);

	async::result<void> (*unlink)(std::shared_ptr<void> object,
			std::string name);

	async::result<OpenResult> (*open)(std::shared_ptr<void> object);

	async::result<std::string> (*readSymlink)(std::shared_ptr<void> object);

	async::result<MkdirResult> (*mkdir)(std::shared_ptr<void> object, std::string name);

	async::result<Error> (*chmod)(std::shared_ptr<void> object, int mode);
};

async::result<void>
serveFile(helix::UniqueLane lane, void *file, const FileOperations *file_ops);

async::result<void> servePassthrough(helix::UniqueLane lane, smarter::shared_ptr<void> file,
		const FileOperations *file_ops, async::cancellation_token cancellation = {});

async::detached serveNode(helix::UniqueLane lane, std::shared_ptr<void> node,
		const NodeOperations *node_ops);


} } // namespace protocols::fs

#endif // LIBFS_SERVER_HPP
