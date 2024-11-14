#include <stddef.h>
#include <stdint.h>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch/paging.hpp>
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

	void *kasanPointerOf(int8_t *shadow) {
		return reinterpret_cast<void *>(
			((reinterpret_cast<uintptr_t>(shadow) - kasanShadowDelta) << kasanShift));
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
#else
	(void)pointer;
	(void)size;
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
#else
	(void)pointer;
	(void)size;
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
#else
	(void)pointer;
	(void)size;
#endif // THOR_KASAN
}

[[gnu::no_sanitize_address]] void validateKasanClean(void *pointer, size_t size) {
#ifdef THOR_KASAN
	assert(!(reinterpret_cast<uintptr_t>(pointer) & (kasanScale - 1)));

	auto shadow = kasanShadowOf(pointer);
	for(size_t n = 0; n < (size >> kasanShift); ++n)
		assert(!shadow[n]);
#else
	(void)pointer;
	(void)size;
#endif // THOR_KASAN
}

void scrubStackFrom(uintptr_t top, Continuation cont) {
	auto bottom = reinterpret_cast<uintptr_t>(cont.sp);
	assert(top >= bottom);
	cleanKasanShadow(cont.sp, top - bottom);
	// Perform some sanity checking.
	validateKasanClean(reinterpret_cast<void *>(bottom & ~(kPageSize - 1)), bottom & (kPageSize - 1));
}

} // namespace thor

#ifdef THOR_KASAN

extern "C" void __asan_alloca_poison(uintptr_t address, size_t size) {
	(void)address;
	(void)size;
	// TODO: Implement alloca poisoning.
}

extern "C" void __asan_allocas_unpoison(void *stackTop, void *stackBottom) {
	(void)stackTop;
	(void)stackBottom;
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
		auto shadow = thor::kasanShadowOf(reinterpret_cast<void *>(address));
		auto l = reinterpret_cast<uintptr_t>(shadow) & 15;
		auto validBehind = (reinterpret_cast<uintptr_t>(shadow) - l) & (thor::kPageSize - 1);
		auto validAhead = thor::kPageSize - validBehind;
		auto shownBehind = frg::min(validBehind, size_t{2 * 16});
		auto shownAhead = frg::min(validAhead, size_t{2 * 16});
		ptrdiff_t i = -static_cast<ptrdiff_t>(shownBehind);
		while(i < static_cast<ptrdiff_t>(16 + shownAhead)) {
			auto msg = thor::infoLogger();
			msg << "thor: Shadow[" << thor::kasanPointerOf(&shadow[-l + i]) << "]:";
			for(size_t j = 0; j < 16; ++j) {
				auto v = static_cast<uint8_t>(shadow[-l + i]);
				msg << (i == l ? "[" : " ")
					<< (v <= 8 ? "0" : "")
					<< frg::hex_fmt(static_cast<unsigned int>(v))
					<< (i == l ? "]" : " ");
				++i;
			}
			msg << frg::endlog;
		}
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
