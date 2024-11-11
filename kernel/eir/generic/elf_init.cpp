#include <eir-internal/debug.hpp>

namespace {

using InitializerPtr = void (*)();
extern "C" InitializerPtr __init_array_start[];
extern "C" InitializerPtr __init_array_end[];
}

namespace eir {

extern "C" void eirRunConstructors() {
	infoLogger() << "There are "
			<< (__init_array_end - __init_array_start) << " constructors" << frg::endlog;
	for(InitializerPtr *p = __init_array_start; p != __init_array_end; ++p)
			(*p)();
}

} // namespace eir
