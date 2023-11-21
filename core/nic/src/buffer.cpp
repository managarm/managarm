#include <algorithm>

#include <async/result.hpp>
#include <hel.h>
#include <hel-syscalls.h>
#include <core/nic/core.hpp>
#include <core/nic/buffer.hpp>
#include <helix/ipc.hpp>

namespace nic_core {

buffer_view buffer_view::fromHelHandle(HelHandle memory, size_t allocatedSize, size_t length, size_t offset) {
	return buffer_view{std::make_shared<buffer_owner>(std::move(memory), allocatedSize, length, offset)};
}

}
