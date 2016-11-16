
#ifndef LIBFS_CLIENT_HPP
#define LIBFS_CLIENT_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unordered_map>

#include <helix/ipc.hpp>
#include <boost/variant.hpp>
#include <cofiber.hpp>
#include <cofiber/future.hpp>

// EVENTUALLY: use std::future instead of cofiber::future!
// EVENTUALLY: use std::variant instead of boost::variant!

namespace protocols {
namespace fs {

namespace _detail {

struct File {
	File(helix::UniqueDescriptor lane);

	helix::BorrowedDescriptor getLane() {
		return _lane;
	}
	
	cofiber::future<void> seekAbsolute(int64_t offset);

	cofiber::future<size_t> readSome(void *data, size_t max_length);
	
	cofiber::future<helix::UniqueDescriptor> accessMemory();

private:
	helix::UniqueDescriptor _lane;
};

} // namespace _detail

using _detail::File;

} } // namespace protocols::fs

#endif // LIBFS_CLIENT_HPP

