#include <cstdio>
#include <cassert>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>

#include "testsuite.hpp"

namespace {
	std::atomic<int> signalFlag = 0;
} // namespace anonymous

constexpr uint64_t magic_expected = 0xDEADBEEFCAFEBABE;

DEFINE_TEST(signal_save_simd, ([] {
	int ret;

#if defined (__x86_64__)
	asm volatile ("movd %0, %%xmm15" : : "r"(magic_expected));
#elif defined (__aarch64__)
	asm volatile ("fmov d31, %0" : : "r"(magic_expected));
#elif defined(__riscv) && __riscv_xlen == 64
	printf("Test is missing support for RISC-V\n");
	__builtin_trap();
#else
#	error Unknown architecture
#endif

	struct sigaction sa;
	memset(&sa, 0, sizeof(struct sigaction));

	sa.sa_flags = SA_RESETHAND;
	sa.sa_handler = [] (int) {
		signalFlag = 1;

#if defined (__x86_64__)
		asm volatile ("movd %0, %%xmm15" : : "r"(uint64_t(0x2BADBADBADBADBAD)));
#elif defined (__aarch64__)
		asm volatile ("fmov d31, xzr");
#elif defined(__riscv) && __riscv_xlen == 64
	printf("Test is missing support for RISC-V\n");
	__builtin_trap();
#else
#	error Unknown architecture
#endif
	};

	ret = sigaction(SIGUSR1, &sa, nullptr);
	assert(ret == 0);

	ret = kill(getpid(), SIGUSR1);
	assert(ret == 0);

	assert(signalFlag == 1);

	uint64_t magic = 0;

#if defined (__x86_64__)
	asm volatile ("movd %%xmm15, %0" : "=r"(magic));
#elif defined (__aarch64__)
	asm volatile ("fmov %0, d31" : "=r"(magic));
#elif defined(__riscv) && __riscv_xlen == 64
	printf("Test is missing support for RISC-V\n");
	__builtin_trap();
#else
#	error Unknown architecture
#endif

	assert(magic == magic_expected);
}))
