
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <helix/ipc.hpp>

namespace helix {

Dispatcher &Dispatcher::global() {
	static Dispatcher dispatcher;
	return dispatcher;
}

async::run_queue *globalQueue() {
	static async::run_queue queue;
	return &queue;
}

} // namespace helix

