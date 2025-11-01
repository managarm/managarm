#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>

#include "testsuite.hpp"

namespace {
	jmp_buf env;
	stack_t ss, old_ss;
} // namespace anonymous

#if !defined(__linux__)

DEFINE_TEST(sigaltstack, ([] {
	if (setjmp(env)) {
		int ret = sigaltstack(&old_ss, nullptr);
		assert(ret == 0);

		operator delete(ss.ss_sp);

		return;
	}

	ss.ss_sp = operator new(SIGSTKSZ);
	ss.ss_size = SIGSTKSZ;
	ss.ss_flags = 0;

	int ret = sigaltstack(&ss, &old_ss);
	assert(ret == 0);

	struct sigaction sa;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sa.sa_sigaction = [] (int, siginfo_t *, void *) {
		longjmp(env, 1);
	};

	ret = sigaction(SIGSEGV, &sa, nullptr);
	assert(ret == 0);

#if defined(__x86_64__)
	asm volatile ("mov $0, %%rsp\n\tpush $0" ::: "rsp");
#elif defined(__aarch64__)
	asm volatile ("mov sp, %0\n\tstp x0, x1, [sp, #-16]!" :: "r"(uint64_t{0}) : "sp");
#elif defined(__riscv) && __riscv_xlen == 64
	printf("Test is missing support for RISC-V\n");
	__builtin_trap();
#else
#	error Unknown architecture
#endif
}))

#endif

DEFINE_TEST(sigaltstack_eperm, ([] {
	if (setjmp(env)) {
		int ret = sigaltstack(&old_ss, nullptr);
		assert(ret == 0);

		operator delete(ss.ss_sp);

		return;
	}

	ss.ss_sp = operator new(SIGSTKSZ);
	ss.ss_size = SIGSTKSZ;
	ss.ss_flags = 0;

	int ret = sigaltstack(&ss, &old_ss);
	assert(ret == 0);

	struct sigaction sa = {};
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sa.sa_sigaction = [] (int, siginfo_t *, void *) {
		stack_t current_ss = {};
		current_ss.ss_sp = operator new(SIGSTKSZ);
		current_ss.ss_size = SIGSTKSZ;
		current_ss.ss_flags = 0;
		int ret = sigaltstack(&current_ss, nullptr);
		assert(ret == -1);
		assert(errno == EPERM);
		operator delete(current_ss.ss_sp);
		longjmp(env, 1);
	};

	ret = sigaction(SIGUSR1, &sa, nullptr);
	assert(ret == 0);

	raise(SIGUSR1);
}))
