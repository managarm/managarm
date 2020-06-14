#include <cassert>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testsuite.hpp"

DEFINE_TEST(fork_exit_waitpid, ([] {
	int pid = fork();
	assert(pid >= 0);
	if(!pid) {
		exit(0);
	}else{
		int status;
		auto res = waitpid(pid, &status, 0);
		assert(res > 0);
	}
}))
