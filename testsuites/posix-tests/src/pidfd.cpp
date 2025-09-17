#include <assert.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>

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

static void *thread_func(void *arg) {
	int evfd = (int)(intptr_t)arg;
	uint64_t val = 1;
	ssize_t ret = write(evfd, &val, sizeof(uint64_t));
	assert(ret == sizeof(uint64_t));
	return nullptr;
}

DEFINE_TEST(pidfd_waitpid_multithread, ([] {
	int evfd = eventfd(0, 0);
	assert(evfd >= 0);

	pid_t child = fork();

	if(!child) {
		pthread_t thread1, thread2;
		pthread_create(&thread1, nullptr, thread_func, (void *)(intptr_t)evfd);
		pthread_create(&thread2, nullptr, thread_func, (void *)(intptr_t)evfd);

		pthread_join(thread1, nullptr);
		pthread_join(thread2, nullptr);
		exit(0);
	}

	int pidfd = pidfd_open(child, 0);
	assert(pidfd > 0);

	siginfo_t si;
	int waited = waitid(P_PIDFD, pidfd, &si, WEXITED | WNOWAIT);
	assert(waited == 0);
	assert(si.si_pid == child);
	assert(si.si_code == CLD_EXITED);
	assert(si.si_status == 0);

	int status;
	pid_t ret = waitpid(child, &status, 0);
	assert(ret == child);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);

	uint64_t val;
	ssize_t bytes_read = read(evfd, &val, sizeof(uint64_t));
	assert(bytes_read == sizeof(uint64_t));
	assert(val == 2);

	close(pidfd);
	close(evfd);
}));
