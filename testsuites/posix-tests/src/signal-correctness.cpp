#include "testsuite.hpp"
#include <cassert>
#include <cerrno>
#include <initializer_list>
#include <signal.h>
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
