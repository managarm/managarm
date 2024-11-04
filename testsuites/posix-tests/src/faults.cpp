#include <cassert>

#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testsuite.hpp"

namespace {
sigjmp_buf restoreEnvFPE;

void signalHandler(int, siginfo_t *, void *) { siglongjmp(restoreEnvFPE, 1); }

bool testDivFault(int a, int b) {
	if (sigsetjmp(restoreEnvFPE, 1)) {
		return true;
	}

	volatile int result = a / b;
	(void)result;

	return false;
}

template <typename Func> void runChecks(Func &&f) {
	pid_t pid = fork();
	assert_errno("fork", pid >= 0);

	struct sigaction sa, old_sa;
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = signalHandler;
	sa.sa_flags = SA_SIGINFO;

	int ret = sigaction(SIGFPE, &sa, &old_sa);
	assert_errno("sigaction", ret != -1);

	if (pid == 0) {
		f();
		exit(0);
	} else {
		int status = 0;
		while (waitpid(pid, &status, 0) == -1) {
			if (errno == EINTR)
				continue;
			assert_errno("waitpid", false);
		}

		if (WIFSIGNALED(status) || WEXITSTATUS(status) != 0) {
			fprintf(stderr, "Test failed on subprocess!\n");
			abort();
		}

		f();
	}

	ret = sigaction(SIGSEGV, &old_sa, nullptr);
	assert_errno("sigaction", ret != -1);
}
} // namespace

DEFINE_TEST(div_by_zero_fpe_fault, ([] {
	            runChecks([&] {
		            assert(testDivFault(1, 0));
		            assert(testDivFault(0, 0));
		            assert(!testDivFault(INT_MIN + 1, -1));
		            assert(testDivFault(INT_MIN, 0));
		            assert(testDivFault(INT_MAX, 0));
	            });
            }))
