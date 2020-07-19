#include <frg/eternal.hpp>
#include <thor-internal/futex.hpp>

namespace thor {

static frg::eternal<FutexSpace> futexSpaceSingleton;

FutexSpace *getGlobalFutexSpace() {
	return &futexSpaceSingleton.get();
}

} // namespace thor
