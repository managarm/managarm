#ifndef LIBFS_COMMON_HPP
#define LIBFS_COMMON_HPP

#include <optional>
#include <variant>

namespace protocols {
namespace fs {

enum class Error {
	none,
	wouldBlock,
	illegalArguments,
	seekOnPipe
};

using ReadResult = std::variant<Error, size_t>;

using ReadEntriesResult = std::optional<std::string>;

using PollResult = std::tuple<uint64_t, int, int>;

} } // namespace protocols::fs

#endif // LIBFS_COMMON_HPP
