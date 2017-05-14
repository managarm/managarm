#ifndef LIBFS_SERVER_HPP
#define LIBFS_SERVER_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <async/result.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>

namespace protocols {
namespace fs {

enum class FileType {
	unknown,
	directory,
	regular
};

using GetLinkResult = std::tuple<std::shared_ptr<void>, FileType>;

struct FileOperations {
	async::result<int64_t> (*seekAbs)(std::shared_ptr<void> object, int64_t offset);
	async::result<int64_t> (*seekRel)(std::shared_ptr<void> object, int64_t offset);
	async::result<int64_t> (*seekEof)(std::shared_ptr<void> object, int64_t offset);
	async::result<size_t> (*read)(std::shared_ptr<void> object, void *buffer, size_t length);
	async::result<void> (*write)(std::shared_ptr<void> object, const void *buffer, size_t length);
	async::result<helix::BorrowedDescriptor> (*accessMemory)(std::shared_ptr<void> object);
};

struct NodeOperations {
	async::result<GetLinkResult> (*getLink)(std::shared_ptr<void> object,
			std::string name);

	async::result<std::shared_ptr<void>> (*open)(std::shared_ptr<void> object);
};

cofiber::no_future servePassthrough(helix::UniqueLane lane, std::shared_ptr<void> node,
		const FileOperations *file_ops);

cofiber::no_future serveNode(helix::UniqueLane lane, std::shared_ptr<void> node,
		const NodeOperations *node_ops, const FileOperations *file_ops);


} } // namespace protocols::fs

#endif // LIBFS_SERVER_HPP
