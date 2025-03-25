#include <assert.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

// I wish I was kidding: glibc does not have C++ guards on this header
#include <sys/pidfd.h>

#ifdef __cplusplus
}
#endif

#include "testsuite.hpp"

DEFINE_TEST(pidfd_poll, ([] {
	int child = fork();

	if(!child) {
		sleep(1);
		exit(42);
	}

	int pidfd = pidfd_open(child, 0);
	assert(pidfd > 0);

	int ret = lseek(pidfd, 0, SEEK_SET);
	assert(ret == -1);
	assert(errno == ESPIPE);

#if defined(__managarm__)
	pid_t outpid = pidfd_getpid(pidfd);
	assert(outpid == child);
#endif

	struct pollfd pollfd;
	pollfd.fd = pidfd;
	pollfd.events = POLLIN;

	int ready = poll(&pollfd, 1, 0);
	assert(ready == 0);

	ready = poll(&pollfd, 1, 2000);
	if(ready == -1) {
		perror("poll");
		exit(EXIT_FAILURE);
	}
	assert(ready == 1);
	assert(pollfd.revents == POLLIN);

	siginfo_t info = {};
	ret = waitid(P_PIDFD, 0, &info, WEXITED | WNOHANG);
	assert(ret == -1);
	assert(errno == EBADF);

	info = {};
	ret = waitid(P_PIDFD, 0xDEAD101, &info, WEXITED | WNOHANG);
	assert(ret == -1);
	assert(errno == EBADF);

	info = {};
	ret = waitid(P_PIDFD, pidfd, &info, WEXITED | WNOHANG);
	assert(ret == 0);
	assert(info.si_code == CLD_EXITED);
	assert(info.si_pid == child);
	assert(info.si_status == 42);

	close(pidfd);
}));
