#include <cassert>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>

#include "testsuite.hpp"

namespace {
jmp_buf env;
stack_t ss, old_ss;
} // namespace

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
	            sa.sa_sigaction = [](int, siginfo_t *, void *) { longjmp(env, 1); };

	            ret = sigaction(SIGSEGV, &sa, nullptr);
	            assert(ret == 0);

#if defined(__x86_64__)
	            asm volatile("mov $0, %%rsp\n\tpush $0" ::: "rsp");
#elif defined(__aarch64__)
	            asm volatile("mov sp, %0\n\tstp x0, x1, [sp, #-16]!" ::"r"(uint64_t{0}) : "sp");
#else
#error Unknown architecture
#endif
            }))
