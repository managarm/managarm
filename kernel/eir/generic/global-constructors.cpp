#include <eir-internal/debug.hpp>

#if !defined(EIR_NATIVE_PE)

using InitializerPtr = void (*)();
// EDK2's GenFw seems to break if these symbols are referenced by GOT relocations on RISC-V.
// Avoid GOT relocations by setting visibility to hidden.
extern "C" [[gnu::visibility("hidden")]] InitializerPtr __init_array_start[];
extern "C" [[gnu::visibility("hidden")]] InitializerPtr __init_array_end[];

extern "C" void eirRunConstructors() {
	auto begin = reinterpret_cast<uintptr_t>(__init_array_start);
	auto end = reinterpret_cast<uintptr_t>(__init_array_end);
	auto count = (end - begin) / sizeof(InitializerPtr);
	eir::infoLogger() << "There are " << count << " constructors" << frg::endlog;
	for (size_t i = 0; i < count; ++i)
		__init_array_start[i]();
}

#else

// MSVC puts global constructors in a section .CRT$XCU that is ordered between .CRT$XCA and
// .CRT$XCZ.
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
