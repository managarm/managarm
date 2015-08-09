
#include "kernel.hpp"
#include "../../hel/include/hel.h"

namespace thor {
namespace k_init {

void main() {
	thorRtDisableInts();
	helLog("Hello\n", 6);
	helExitThisThread();
}

} } // namespace thor::k_init

