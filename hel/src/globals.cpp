
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <helix/ipc.hpp>

namespace helix {

namespace {
	// Wrapper around Dispatcher to set the default queue.
	struct GlobalDispatcher {
		GlobalDispatcher() {
			async::current_run_queue_context()->set_default_queue(dispatcher.runQueue());
		}

		~GlobalDispatcher() {
			async::current_run_queue_context()->reset_default_queue();
		}

		Dispatcher dispatcher;
	};
}

Dispatcher &Dispatcher::global() {
	thread_local static GlobalDispatcher singleton;
	return singleton.dispatcher;
}

} // namespace helix

