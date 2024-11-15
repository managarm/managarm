#include <eir-internal/debug.hpp>

#if !defined(EIR_NATIVE_PE)

using InitializerPtr = void (*)();
extern "C" InitializerPtr __init_array_start[];
extern "C" InitializerPtr __init_array_end[];

extern "C" void eirRunConstructors() {
	eir::infoLogger() << "There are "
			<< (__init_array_end - __init_array_start) << " constructors" << frg::endlog;
	for(InitializerPtr *p = __init_array_start; p != __init_array_end; ++p)
			(*p)();
}

#else

// MSVC puts global constructors in a section .CRT$XCU that is ordered between .CRT$XCA and .CRT$XCZ.
__declspec(allocate(".CRT$XCA")) const void *crt_xct = nullptr;
__declspec(allocate(".CRT$XCZ")) const void *crt_xcz = nullptr;

extern "C" void eirRunConstructors() {
	using InitializerPtr = void (*)();
	uintptr_t begin = reinterpret_cast<uintptr_t>(&crt_xct);
	uintptr_t end = reinterpret_cast<uintptr_t>(&crt_xcz);
	for (uintptr_t it = begin + sizeof(void *); it < end; it += sizeof(void *)) {
		auto *p = reinterpret_cast<InitializerPtr *>(it);
		(*p)();
	}
}

#endif
