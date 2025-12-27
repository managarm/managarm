#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "testsuite.hpp"

DEFINE_TEST(pty_master_hangup, ([] {
	int master;
	int slavefd;
	char slave_path[256];
	if(openpty(&master, &slavefd, slave_path, nullptr, nullptr)) {
		fprintf(stderr, "openpty() failed: %s\n", strerror(errno));
		assert(!"openpty failed");
	}

	int masterdup = dup(master);

	int slave = open(slave_path, O_RDWR);
	assert(slave != -1);

	struct pollfd pfd;
	pfd.fd = slave;
	pfd.events = POLLIN;
	pfd.revents = 0;

	int ret = poll(&pfd, 1, 0);
	assert(ret == 0);

	close(master);

	ret = poll(&pfd, 1, 0);
	assert(ret == 0);

	close(masterdup);

	ret = poll(&pfd, 1, 0);
	assert(ret == 1);
	assert(pfd.revents & POLLHUP);

	close(slave);
	close(slavefd);
}))

DEFINE_TEST(pty_slave_hangup, ([] {
	int master;
	int slavefd;
	char slave_path[256];
	if(openpty(&master, &slavefd, slave_path, nullptr, nullptr)) {
		fprintf(stderr, "openpty() failed: %s\n", strerror(errno));
		assert(!"openpty failed");
	}

	int slave = open(slave_path, O_RDWR);
	assert(slave != -1);

	struct pollfd pfd;
	pfd.fd = master;
	pfd.events = POLLIN;
	pfd.revents = 0;

	int ret = poll(&pfd, 1, 0);
	// Before closing slave, poll should not return POLLHUP.
	assert(!ret);
	assert(!(pfd.revents & POLLHUP));

	close(slave);

	ret = poll(&pfd, 1, 0);
	// Before closing slave, poll should not return POLLHUP.
	assert(!ret);
	assert(!(pfd.revents & POLLHUP));

	close(slavefd);

	// After closing slave, poll should return POLLHUP.
	ret = poll(&pfd, 1, 0);
	assert(ret >= 0);
	assert(pfd.revents & POLLHUP);

	close(master);
}))

DEFINE_TEST(pty_sigwinch, ([] {
	int master;
	int slavefd;
	char slave_path[256];
	if(openpty(&master, &slavefd, slave_path, nullptr, nullptr)) {
		fprintf(stderr, "openpty() failed: %s\n", strerror(errno));
		assert(!"openpty failed");
	}

	pid_t child = fork();
	assert(child >= 0);

	if (!child) {
		close(master);
		auto sid = setsid();
		assert(sid >= 0);

		int ret = ioctl(slavefd, TIOCSCTTY, 0);
		assert(ret == 0);

		struct sigaction sa = {};
		sa.sa_handler = SIG_IGN;
		ret = sigaction(SIGWINCH, &sa, NULL);
		assert(!ret);

		sigset_t sigwinch_mask;
		sigemptyset(&sigwinch_mask);
		sigaddset(&sigwinch_mask, SIGWINCH);

		sigset_t oldmask;
		sigemptyset(&oldmask);
		sigprocmask(SIG_BLOCK, &sigwinch_mask, &oldmask);

		struct winsize ws = { .ws_row = 24, .ws_col = 80 };
		ret = ioctl(slavefd, TIOCSWINSZ, &ws);
		assert(!ret);

		struct timespec ten_sec = {
			.tv_sec = 3,
			.tv_nsec = 0,
		};
		ret = sigtimedwait(&sigwinch_mask, nullptr, &ten_sec);
		assert(ret == SIGWINCH);

		struct winsize new_ws;
		ret = ioctl(slavefd, TIOCGWINSZ, &new_ws);
		assert(!ret);

		assert(new_ws.ws_row == ws.ws_row && new_ws.ws_col == ws.ws_col);

		exit(0);
	}

	close(slavefd);

	int status = 0;
	int ret = waitpid(child, &status, 0);
	assert(ret >= 0);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);
}))
