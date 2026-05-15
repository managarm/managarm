#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/thread.hpp>

namespace thor {

// Block on a sender without specifying a work queue.
// Because no WQ is specified, this only supports senders that do not need to schedule work.
// For example, coroutines are *not* supported.
template<typename S>
typename S::value_type blockOn(S sender) {
	if (getCurrentThread()) {
		Thread::asyncBlockCurrent(std::move(sender), nullptr);
	} else if(thisFiber()) {
		KernelFiber::asyncBlockCurrent(std::move(sender), nullptr);
	} else {
		panicLogger() << "thor: Trying to block outside of thread-like context" << frg::endlog;
	}
}

} // namespace thor
