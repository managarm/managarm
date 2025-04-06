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
