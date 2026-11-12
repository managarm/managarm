#include "testsuite.hpp"
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <initializer_list>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

template <typename F>
void checkSignalInChild(F check) {
	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		alarm(5);
		check();
		_exit(0);
	}
	int status;
	pid_t result;
	do {
		result = waitpid(child, &status, 0);
	} while (result < 0 && errno == EINTR);
	assert(result == child && WIFEXITED(status) && !WEXITSTATUS(status));
}

} // namespace

DEFINE_TEST(signal_high_number_mask, ([] {
	            checkSignalInChild([] {
		            static int signo = SIGRTMIN;
		            static volatile sig_atomic_t blocked = -1;
		            assert(signo > 32 && signo <= SIGRTMAX);
		            for (bool nodefer : {false, true}) {
			            struct sigaction action{};
			            action.sa_handler = [](int) {
				            sigset_t mask;
				            assert(!sigprocmask(SIG_BLOCK, nullptr, &mask));
				            blocked = sigismember(&mask, signo);
			            };
			            action.sa_flags = nodefer ? SA_NODEFER : 0;
			            assert(!sigemptyset(&action.sa_mask));
			            assert(!sigaction(signo, &action, nullptr));
			            blocked = -1;
			            assert(!raise(signo));
			            assert(blocked == !nodefer);
		            }
	            });
            }))

DEFINE_TEST(
    sigaltstack_request_errors, ([] {
	    checkSignalInChild([] {
		    auto pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
		    auto inaccessible =
		        mmap(nullptr, pageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		    assert(inaccessible != MAP_FAILED);
		    auto bad = static_cast<stack_t *>(inaccessible);
		    stack_t original{};
		    assert(!sigaltstack(nullptr, &original));
		    auto buffer =
		        mmap(nullptr, SIGSTKSZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		    assert(buffer != MAP_FAILED);
		    assert(!munmap(inaccessible, pageSize));
		    stack_t stack{};
		    stack.ss_sp = buffer;
		    stack.ss_size = SIGSTKSZ;
		    errno = 0;
		    assert(sigaltstack(&stack, bad) == -1 && errno == EFAULT);
		    stack_t current{};
		    assert(!sigaltstack(nullptr, &current));
#if !defined(__linux__)
		    assert((current.ss_flags & SS_DISABLE) == (original.ss_flags & SS_DISABLE));
#endif
		    assert(!sigaltstack(&original, nullptr));
#if defined(__linux__)
		    errno = 0;
		    assert(sigaltstack(bad, nullptr) == -1 && errno == EFAULT);
#endif
		    stack.ss_size = 1;
		    errno = 0;
		    assert(sigaltstack(&stack, nullptr) == -1 && errno == ENOMEM);

		    // Disabled stacks must not report SS_ONSTACK or prevent replacement,
		    // even when their supplied bounds surround the current stack pointer.
		    stack.ss_flags = SS_DISABLE;
		    auto page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
		    auto base = reinterpret_cast<uintptr_t>(&stack) & ~(page - 1);
		    stack.ss_sp = reinterpret_cast<void *>(base - 8 * page);
		    stack.ss_size = 16 * page;
		    assert(!sigaltstack(&stack, nullptr));
		    assert(!sigaltstack(nullptr, &current));
		    assert(current.ss_flags == SS_DISABLE);
		    stack.ss_flags = 0;
		    stack.ss_sp = buffer;
		    stack.ss_size = SIGSTKSZ;
		    assert(!sigaltstack(&stack, nullptr));
		    assert(!sigaltstack(&original, nullptr));
		    assert(!munmap(buffer, SIGSTKSZ));
	    });
    })
)

DEFINE_TEST(illegal_instruction_signal_information, ([] {
	            checkSignalInChild([] {
		            static sigjmp_buf env;
		            static volatile sig_atomic_t code = 0;
		            static volatile uintptr_t address = 0;
		            struct sigaction action{};
		            action.sa_flags = SA_SIGINFO;
		            action.sa_sigaction = [](int signo, siginfo_t *info, void *) {
			            assert(signo == SIGILL);
			            code = info->si_code;
			            address = reinterpret_cast<uintptr_t>(info->si_addr);
			            siglongjmp(env, 1);
		            };
		            assert(!sigemptyset(&action.sa_mask));
		            assert(!sigaction(SIGILL, &action, nullptr));
		            if (!sigsetjmp(env, 1)) {
#if defined(__x86_64__)
			            asm volatile("ud2");
#elif defined(__aarch64__)
			            asm volatile(".inst 0");
#else
			            asm volatile(".word 0");
#endif
			            assert(!"illegal instruction returned");
		            }
		            assert(address != 0);
		            assert(code == ILL_ILLOPC || code == ILL_ILLOPN);
	            });
            }))

DEFINE_TEST(integer_divide_signal_information, ([] {
#if defined(__x86_64__)
	            checkSignalInChild([] {
		            static sigjmp_buf env;
		            static volatile sig_atomic_t code = 0;
		            static volatile uintptr_t address = 0;
		            struct sigaction action{};
		            action.sa_flags = SA_SIGINFO;
		            action.sa_sigaction = [](int signo, siginfo_t *info, void *) {
			            assert(signo == SIGFPE);
			            code = info->si_code;
			            address = reinterpret_cast<uintptr_t>(info->si_addr);
			            siglongjmp(env, 1);
		            };
		            assert(!sigemptyset(&action.sa_mask));
		            assert(!sigaction(SIGFPE, &action, nullptr));
		            if (!sigsetjmp(env, 1)) {
			            asm volatile("xor %%edx, %%edx; divl %%ecx"
			                         :
			                         : "a"(1), "c"(0)
			                         : "edx", "cc");
			            assert(!"divide by zero returned");
		            }
		            assert(address != 0 && code == FPE_INTDIV);
	            });
#else
	            skip_test("test needs an x86 integer divide fault");
#endif
            }))
