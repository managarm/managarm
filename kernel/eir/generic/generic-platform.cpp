// This is a generic platform without any special initialization code.
// Boot protocols link against this if required.

#include <eir-internal/arch.hpp>

namespace eir {

void debugPrintChar(char c) { (void)c; }

void initPlatform() {}

} // namespace eir
