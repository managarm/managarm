#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
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

DEFINE_TEST(poll_same_fd_pollfd, ([] {
	int sv[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) {
		perror("socketpair");
		exit(1);
	}

	struct pollfd fds[2];
	fds[0].fd = sv[0];
	fds[0].events = POLLIN;
	fds[0].revents = 0;

	fds[1].fd = sv[0];
	fds[1].events = POLLOUT;
	fds[1].revents = 0;

	int ret = poll(fds, 2, 1000); // 1 second timeout
	assert(ret == 1);
	assert(fds[0].revents == 0);
	assert(fds[1].revents == POLLOUT);

	fds[0].events = POLLIN | POLLOUT;

	ret = poll(fds, 2, 1000); // 1 second timeout
	assert(ret == 2);
	assert(fds[0].revents == POLLOUT);
	assert(fds[1].revents == POLLOUT);

	char buffer = 'X';
	if (write(sv[1], &buffer, 1) != 1) {
		perror("write");
		exit(1);
	}

	fds[0].fd = sv[0];
	fds[0].events = POLLIN;
	fds[0].revents = 0;

	fds[1].fd = sv[0];
	fds[1].events = POLLOUT;
	fds[1].revents = 0;

	ret = poll(fds, 2, 1000); // 1 second timeout
	assert(ret == 2);
	assert(fds[0].revents == POLLIN);
	assert(fds[1].revents == POLLOUT);

	close(sv[0]);
	close(sv[1]);

	return 0;
}))
