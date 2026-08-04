#include <cassert>
#include <sys/wait.h>
#include <unistd.h>
#include <poll.h>

#include "testsuite.hpp"

DEFINE_TEST(dup2_closes_replaced_pipe_writer, ([] {
	int first[2];
	int second[2];
	assert(pipe(first) == 0);
	assert(pipe(second) == 0);

	pid_t child = fork();
	assert(child >= 0);
	if(!child) {
		close(first[1]);
		close(second[0]);
		close(second[1]);

		pollfd pfd{first[0], POLLIN | POLLHUP, 0};
		int result = poll(&pfd, 1, 1000);
		_exit(result == 1 && (pfd.revents & POLLHUP) ? 0 : 1);
	}

	close(first[0]);
	close(second[0]);
	assert(dup2(second[1], first[1]) == first[1]);
	close(second[1]);
	close(first[1]);

	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);
}))

DEFINE_TEST(dup2_same_fd_is_noop, ([] {
	int fds[2];
	assert(pipe(fds) == 0);
	assert(dup2(fds[1], fds[1]) == fds[1]);
	close(fds[1]);

	pollfd pfd{fds[0], POLLIN | POLLHUP, 0};
	assert(poll(&pfd, 1, 0) == 1);
	assert(pfd.revents & POLLHUP);
	close(fds[0]);
}))
