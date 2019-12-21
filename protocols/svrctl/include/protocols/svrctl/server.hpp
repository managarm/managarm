#ifndef LIBSVRCTL_PROTOCOL_SERVER_HPP
#define LIBSVRCTL_PROTOCOL_SERVER_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <async/cancellation.hpp>
#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>
#include <smarter.hpp>

namespace protocols {
namespace svrctl {

enum class Error {
	success,
	deviceNotSupported = 2
};

struct ControlOperations {
	// Tries to bind to a device with a given mbus ID.
	// Returns Error::deviceNotSupported if the operation is not possible.
	async::result<Error> (*bind)(int64_t base_id);
};

async::result<void>
serveControl(const ControlOperations *ops);

} } // namespace protocols::svrctl

#endif // LIBSVRCTL_PROTOCOL_SERVER_HPP
