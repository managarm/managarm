
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <helix/ipc.hpp>

namespace helix {

Dispatcher &Dispatcher::global() {
	thread_local static Dispatcher dispatcher;
	return dispatcher;
}

async::run_queue *globalQueue() {
	thread_local static async::run_queue queue;
	return &queue;
}

} // namespace helix

