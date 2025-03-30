#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/poll.h>

#include "testsuite.hpp"

DEFINE_TEST(socket_shutdown_wr, ([] {
	int fds[2];
	int ret = socketpair(AF_UNIX, SOCK_DGRAM, 0, fds);
	assert(!ret);

	struct pollfd pfd;
	pfd.fd = fds[1];
	pfd.events = POLLIN | POLLPRI | POLLOUT | POLLRDHUP | POLLERR | POLLHUP | POLLNVAL;
	ret = poll(&pfd, 1, 0);
	assert(ret == 1);
	assert(pfd.revents == (POLLOUT));

	ret = write(fds[0], &fds[0], sizeof(fds[0]));
	assert(ret == sizeof(fds[0]));

	ret = shutdown(fds[1], SHUT_WR);
	assert(!ret);

	ret = poll(&pfd, 1, 0);
	assert(ret == 1);
	assert(pfd.revents == (POLLIN | POLLOUT));

	pfd.fd = fds[0];
	ret = poll(&pfd, 1, 0);
	assert(ret == 1);
	assert(pfd.revents == (POLLOUT));

	int discard;
	ret = send(fds[1], &discard, sizeof(discard), MSG_NOSIGNAL);
	ret = send(fds[1], &discard, sizeof(discard), MSG_NOSIGNAL);
	assert(ret == -1);
	assert(errno == EPIPE);
	close(fds[0]);
	close(fds[1]);
}));

DEFINE_TEST(socket_shutdown_rd, ([] {
	int fds[2];
	int ret = socketpair(AF_UNIX, SOCK_DGRAM, 0, fds);
	assert(!ret);
	ret = write(fds[0], &fds[0], sizeof(fds[0]));
	assert(ret == sizeof(fds[0]));

	struct pollfd pfd;
	pfd.fd = fds[1];
	pfd.events = POLLIN | POLLPRI | POLLOUT | POLLRDHUP | POLLERR | POLLHUP | POLLNVAL;
	ret = poll(&pfd, 1, 0);
	assert(ret == 1);
	assert(pfd.revents == (POLLIN | POLLOUT));

	ret = shutdown(fds[1], SHUT_RD);
	assert(!ret);

	ret = poll(&pfd, 1, 0);
	assert(ret == 1);
	assert(pfd.revents == (POLLIN | POLLOUT | POLLRDHUP));

	pfd.fd = fds[0];
	ret = poll(&pfd, 1, 0);
	assert(ret == 1);
	assert(pfd.revents == (POLLOUT));

	int discard;
	ret = recv(fds[1], &discard, sizeof(discard), 0);
	ret = recv(fds[1], &discard, sizeof(discard), 0);
	assert(ret == 0);
	close(fds[0]);
	close(fds[1]);
}));
