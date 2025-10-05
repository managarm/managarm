#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "testsuite.hpp"

DEFINE_TEST(poll_close, ([] {
	int fds[2];
	int e = pipe(fds);
	assert(!e);
	close(fds[0]);

	pollfd pfd[3];
	memset(&pfd, 0, sizeof(pollfd));
	pfd[0].fd = fds[0];
	pfd[1].fd = fds[1];
	pfd[1].events = POLLIN | POLLOUT;
	pfd[2].fd = -1;
	e = poll(pfd, 3, -1);
	assert(e == 2);
	assert(pfd[0].revents == POLLNVAL);
	assert(pfd[1].revents == (POLLOUT | POLLERR));
}))

namespace {

void handler(int) {
	abort();
}

}

DEFINE_TEST(poll_signal, ([] {
	signal(SIGUSR1, handler);
	sigset_t sigusr1;
	sigemptyset(&sigusr1);
	sigaddset(&sigusr1, SIGUSR1);
	sigprocmask(SIG_BLOCK, &sigusr1, NULL);
	sigset_t empty;
	sigemptyset(&empty);
	int fds[2];
	int ret = pipe(fds);
	assert(ret == 0);
	close(fds[0]);
	struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
	ret = ppoll(&pfd, 1, NULL, &empty);
	assert(ret == 1);
	assert(pfd.revents == POLLNVAL);
}))
