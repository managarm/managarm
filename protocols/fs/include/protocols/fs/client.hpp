#ifndef LIBFS_CLIENT_HPP
#define LIBFS_CLIENT_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unordered_map>

#include <async/result.hpp>
#include <boost/variant.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>

// EVENTUALLY: use std::variant instead of boost::variant!

namespace protocols {
namespace fs {

namespace _detail {

struct File {
	File(helix::UniqueDescriptor lane);

	helix::BorrowedDescriptor getLane() {
		return _lane;
	}
	
	async::result<void> seekAbsolute(int64_t offset);

	async::result<size_t> readSome(void *data, size_t max_length);
	
	async::result<helix::UniqueDescriptor> accessMemory();

private:
	helix::UniqueDescriptor _lane;
};

} // namespace _detail

using _detail::File;

} } // namespace protocols::fs

#endif // LIBFS_CLIENT_HPP
