#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unordered_map>

#include <async/cancellation.hpp>
#include <async/result.hpp>
#include <boost/variant.hpp>
#include <frg/expected.hpp>
#include <helix/ipc.hpp>
#include <protocols/fs/common.hpp>

// EVENTUALLY: use std::variant instead of boost::variant!

namespace protocols {
namespace fs {

namespace _detail {

struct File {
	File(helix::UniqueDescriptor lane);

	helix::BorrowedDescriptor getLane() { return _lane; }

	async::result<void> seekAbsolute(int64_t offset);

	async::result<size_t> readSome(void *data, size_t max_length);
	async::result<size_t> writeSome(const void *data, size_t max_length);

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(uint64_t sequence, int mask, async::cancellation_token cancellation = {});

	async::result<frg::expected<Error, PollStatusResult>> pollStatus();

	async::result<helix::UniqueDescriptor> accessMemory();

  private:
	helix::UniqueDescriptor _lane;
};

} // namespace _detail

using _detail::File;

} // namespace fs
} // namespace protocols
