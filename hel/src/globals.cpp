
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <helix/ipc.hpp>

namespace helix {

Dispatcher &Dispatcher::global() {
	static Dispatcher dispatcher;
	return dispatcher;
}

} // namespace helix

