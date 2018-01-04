#ifndef LIBFS_COMMON_HPP
#define LIBFS_COMMON_HPP

namespace protocols {
namespace fs {

using PollResult = std::tuple<uint64_t, int, int>;

} } // namespace protocols::fs

#endif // LIBFS_COMMON_HPP
