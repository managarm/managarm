#include <assert.h>
#include <errno.h>
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

DEFINE_TEST(socket_so_peek_off, [] {
	int fd[2];
	int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fd);
	assert(!ret);

	const char *test = "aabbccddeeff";
	ssize_t written = write(fd[1], test, strlen(test));
	assert(written > 0 && static_cast<size_t>(written) == strlen(test));

	int off = 4;
	ret = setsockopt(fd[0], SOL_SOCKET, SO_PEEK_OFF, &off, sizeof(off));

	char buf[2];
	memset(buf, 0, sizeof(buf));

	ret = recv(fd[0], buf, 2, MSG_PEEK | MSG_DONTWAIT);
	assert(ret == 2);
	assert(buf[0] == 'c' && buf[1] == 'c');
	socklen_t off_len = sizeof(off);
	ret = getsockopt(fd[0], SOL_SOCKET, SO_PEEK_OFF, &off, &off_len);
	assert(ret == 0);
	assert(off_len == sizeof(off));
	assert(off == 6);

	ret = recv(fd[0], buf, 2, MSG_PEEK | MSG_DONTWAIT);
	assert(ret == 2);
	assert(buf[0] == 'd' && buf[1] == 'd');
	ret = getsockopt(fd[0], SOL_SOCKET, SO_PEEK_OFF, &off, &off_len);
	assert(ret == 0);
	assert(off_len == sizeof(off));
	assert(off == 8);

	ret = recv(fd[0], buf, 2, MSG_DONTWAIT);
	assert(ret == 2);
	assert(buf[0] == 'a' && buf[1] == 'a');
	ret = getsockopt(fd[0], SOL_SOCKET, SO_PEEK_OFF, &off, &off_len);
	assert(ret == 0);
	assert(off_len == sizeof(off));
	assert(off == 6);

	ret = recv(fd[0], buf, 2, MSG_PEEK | MSG_DONTWAIT);
	assert(ret == 2);
	assert(buf[0] == 'e' && buf[1] == 'e');
	ret = getsockopt(fd[0], SOL_SOCKET, SO_PEEK_OFF, &off, &off_len);
	assert(ret == 0);
	assert(off_len == sizeof(off));
	assert(off == 8);
});
