#include <stddef.h>
#include <stdint.h>
#include <frigg/debug.hpp>
#include <thor-internal/debug.hpp>

namespace thor {

#ifdef THOR_KASAN
namespace {
	constexpr int kasanShift = 3;
	constexpr uintptr_t kasanShadowDelta = 0xdfffe00000000000;
	constexpr bool debugKasan = false;

	constexpr size_t kasanScale = size_t{1} << kasanShift;

	int8_t *kasanShadowOf(void *ptr) {
		return reinterpret_cast<int8_t *>(
			kasanShadowDelta + (reinterpret_cast<uintptr_t>(ptr) >> kasanShift));
	}
}
#endif // THOR_KASAN

[[gnu::no_sanitize_address]] void unpoisonKasanShadow(void *pointer, size_t size) {
#ifdef THOR_KASAN
	assert(!(reinterpret_cast<uintptr_t>(pointer) & (kasanScale - 1)));
	auto shadow = kasanShadowOf(pointer);
	if(debugKasan)
		infoLogger() << "thor: Unpoisioning KASAN at " << pointer
				<< ", size: " << (void *)size << frg::endlog;
	for(size_t n = 0; n < (size >> kasanShift); ++n) {
		assert(shadow[n] == static_cast<int8_t>(0xFF));
		shadow[n] = 0;
	}
	if(size & (kasanScale - 1)) {
		assert(shadow[size >> kasanShift] == static_cast<int8_t>(0xFF));
		shadow[size >> kasanShift] = size & (kasanScale - 1);
	}
#endif // THOR_KASAN
}

[[gnu::no_sanitize_address]] void poisonKasanShadow(void *pointer, size_t size) {
#ifdef THOR_KASAN
	assert(!(reinterpret_cast<uintptr_t>(pointer) & (kasanScale - 1)));
	auto shadow = kasanShadowOf(pointer);
	if(debugKasan)
		infoLogger() << "thor: Poisioning KASAN at " << pointer
				<< ", size: " << (void *)size << frg::endlog;
	for(size_t n = 0; n < (size >> kasanShift); ++n) {
		assert(shadow[n] == static_cast<int8_t>(0x00));
		shadow[n] = 0xFF;
	}
	if(size & (kasanScale - 1)) {
		assert(shadow[size >> kasanShift] == (size & (kasanScale - 1)));
		shadow[size >> kasanShift] = 0xFF;
	}
#endif // THOR_KASAN
}

[[gnu::no_sanitize_address]] void cleanKasanShadow(void *pointer, size_t size) {
#ifdef THOR_KASAN
	assert(!(reinterpret_cast<uintptr_t>(pointer) & (kasanScale - 1)));
	if(debugKasan)
		infoLogger() << "thor: Cleaning KASAN at " << pointer
				<< ", size: " << (void *)size << frg::endlog;
	auto shadow = kasanShadowOf(pointer);
	for(size_t n = 0; n < (size >> kasanShift); ++n)
		shadow[n] = 0;
	if(size & (kasanScale - 1))
		shadow[size >> kasanShift] = size & (kasanScale - 1);
#endif // THOR_KASAN
}

} // namespace thor

#ifdef THOR_KASAN

extern "C" void __asan_alloca_poison(uintptr_t address, size_t size) {
	// TODO: Implement alloca poisoning.
}

extern "C" void __asan_allocas_unpoison(void *stackTop, void *stackBottom) {
	// TODO: Implement alloca poisoning.
}

extern "C" [[gnu::no_sanitize_address]]
void __asan_set_shadow_00(void *pointer, size_t size) {
	auto p = reinterpret_cast<int8_t *>(pointer);
	for(size_t n = 0; n < size; ++n)
		p[n] = 0;
}

namespace {
	[[gnu::no_sanitize_address]]
	void doReport(bool write, uintptr_t address, size_t size, void *ip) {
		thor::infoLogger() << "thor: KASAN failure at IP " << ip << ", "
				<< size << "-byte "
				<< (write ? "write to " : "read from ")
				<< "address " << (void *)address << frg::endlog;
		uint8_t value = *thor::kasanShadowOf(reinterpret_cast<void *>(address));
		thor::infoLogger() << "thor: Shadow value is "
				<< frg::hex_fmt(static_cast<unsigned int>(value)) << frg::endlog;
		thor::panic();
	}
}

extern "C" void __asan_report_load1_noabort(uintptr_t address) {
	doReport(false, address, 1, __builtin_return_address(0));
}
extern "C" void __asan_report_load2_noabort(uintptr_t address) {
	doReport(false, address, 2, __builtin_return_address(0));
}
extern "C" void __asan_report_load4_noabort(uintptr_t address) {
	doReport(false, address, 4, __builtin_return_address(0));
}
extern "C" void __asan_report_load8_noabort(uintptr_t address) {
	doReport(false, address, 8, __builtin_return_address(0));
}
extern "C" void __asan_report_load16_noabort(uintptr_t address) {
	doReport(false, address, 16, __builtin_return_address(0));
}

extern "C" void __asan_report_load_n_noabort(uintptr_t address, size_t size) {
	doReport(false, address, size, __builtin_return_address(0));
}

extern "C" void __asan_report_store1_noabort(uintptr_t address) {
	doReport(true, address, 1, __builtin_return_address(0));
}
extern "C" void __asan_report_store2_noabort(uintptr_t address) {
	doReport(true, address, 2, __builtin_return_address(0));
}
extern "C" void __asan_report_store4_noabort(uintptr_t address) {
	doReport(true, address, 4, __builtin_return_address(0));
}
extern "C" void __asan_report_store8_noabort(uintptr_t address) {
	doReport(true, address, 8, __builtin_return_address(0));
}
extern "C" void __asan_report_store16_noabort(uintptr_t address) {
	doReport(true, address, 16, __builtin_return_address(0));
}

extern "C" void __asan_report_store_n_noabort(uintptr_t address, size_t size) {
	doReport(false, address, size, __builtin_return_address(0));
}

extern "C" void __asan_handle_no_return() {
	// Do nothing (Linux leaves this empty as well).
}

#endif // THOR_KASAN
