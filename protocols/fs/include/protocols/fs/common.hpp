#ifndef LIBFS_COMMON_HPP
#define LIBFS_COMMON_HPP

#include <optional>
#include <variant>
#include <vector>

namespace protocols {
namespace fs {

enum class Error {
	none,
	wouldBlock,
	illegalArguments,
	seekOnPipe,
	brokenPipe
};

using ReadResult = std::variant<Error, size_t>;

using ReadEntriesResult = std::optional<std::string>;

using PollResult = std::tuple<uint64_t, int, int>;

struct RecvData {
	size_t dataLength;
	size_t addressLength;
	std::vector<char> ctrl;
};

using RecvResult = std::variant<Error, RecvData>;
using SendResult = std::variant<Error, size_t>;

} } // namespace protocols::fs

#endif // LIBFS_COMMON_HPP
