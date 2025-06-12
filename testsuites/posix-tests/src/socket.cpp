#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/poll.h>
#include <sys/un.h>
#include <unistd.h>

#include "testsuite.hpp"

DEFINE_TEST(socket_accept_timeout, ([] {
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un local;
	local.sun_family = AF_UNIX;
	strcpy(local.sun_path, "/tmp/testsocket");
	unlink("/tmp/testsocket");

	struct timespec timeout = {1, 0};
	int ret = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	assert(!ret);

	size_t len = strlen(local.sun_path) + sizeof(local.sun_family);
	ret = bind(s, (struct sockaddr *) &local, len);
	assert(!ret);

	ret = listen(s, 1);
	assert(!ret);

	struct timeval before;
	gettimeofday(&before, nullptr);

	struct sockaddr_un remote;
	socklen_t sock_len;
	ret = accept(s, (struct sockaddr *) &remote, &sock_len);
	assert(ret == -1);
	assert(errno == EAGAIN || errno == EWOULDBLOCK);

	struct timeval after;
	gettimeofday(&after, nullptr);

	struct timeval diff = {0, 0};
	timersub(&after, &before, &diff);
	fprintf(stderr, "accept() waited for %ld.%06ld sec\n", diff.tv_sec, diff.tv_usec);
	assert(diff.tv_sec >= 1);

	unlink("/tmp/testsocket");
}));

DEFINE_TEST(socket_invalid_types, ([] {
	int s = socket(AF_UNIX, 0, 0);
	assert(s == -1);
	assert(errno == ESOCKTNOSUPPORT);

	s = socket(AF_UNIX, INT_MAX, 0);
	assert(s == -1);
	assert(errno == EINVAL);
}));

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
