#include <assert.h>
#include <err.h>
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

// From https://gist.github.com/netbsduser/b219af354dbe01083f7a1c57ac2c531a
DEFINE_TEST(socket_msg_boundary, ([] {
	int sock[2];
	int r;
	struct msghdr msg;
	struct iovec iov;
	char cmsgbuf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;
	char buf[10];

	r = socketpair(PF_UNIX, SOCK_STREAM, 0, sock);
	if (r < 0)
		err(EXIT_FAILURE, "socketpair");

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);
	msg.msg_flags = 0;

	iov.iov_base = (char *) "Hello";
	iov.iov_len = 5;

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	*((int *)CMSG_DATA(cmsg)) = 0;

	r = sendmsg(sock[0], &msg, 0);
	if (r < 0)
		err(EXIT_FAILURE, "sendmsg 1");

	r = sendmsg(sock[0], &msg, 0);
	if (r < 0)
		err(EXIT_FAILURE, "sendmsg 2");

	iov.iov_base = buf;
	iov.iov_len = 10;

	r = recvmsg(sock[1], &msg, 0);
	if (r < 0)
		err(EXIT_FAILURE, "recvmsg");

	printf("recvmsg returned %d, should be 5; controllen is %zu\n", r,
	    msg.msg_controllen);
	assert(r == 5);

	iov.iov_len = 5;
	r = recvmsg(sock[1], &msg, 0);
	if (r < 0)
		err(EXIT_FAILURE, "recvmsg");

	printf("received remaining %d bytes; controllen is %zu\n", r,
	    msg.msg_controllen);
}));

// From https://gist.github.com/netbsduser/b219af354dbe01083f7a1c57ac2c531a
DEFINE_TEST(socket_msg_partial_read, ([] {
	int sock[2];
	int r;
	struct msghdr msg;
	struct iovec iov;
	char cmsgbuf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;
	char buf[10];

	r = socketpair(PF_UNIX, SOCK_STREAM, 0, sock);
	if (r < 0)
		err(EXIT_FAILURE, "socketpair");

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);
	msg.msg_flags = 0;

	iov.iov_base = (char *) "Hello";
	iov.iov_len = 5;

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	*((int *)CMSG_DATA(cmsg)) = 0;

	r = sendmsg(sock[0], &msg, 0);
	if (r < 0)
		err(EXIT_FAILURE, "sendmsg");

	/* this should dispose of the rights */
	r = read(sock[1], buf, 1);
	if (r < 0)
		err(EXIT_FAILURE, "read");

	iov.iov_base = buf;
	iov.iov_len = sizeof(buf) - 1;

	r = recvmsg(sock[1], &msg, 0);
	if (r < 0)
		err(EXIT_FAILURE, "recvmsg");

	printf("recvmsg returned %d bytes; controllen is %zu (should be 0)\n",
		r, msg.msg_controllen);
	assert(msg.msg_controllen == 0);
}));

// From https://gist.github.com/netbsduser/b219af354dbe01083f7a1c57ac2c531a
DEFINE_TEST(socket_msg_fd_truncation, ([] {
	int sock[2];
	int r;
	struct msghdr msg;
	struct iovec iov;
	char send_cmsgbuf[CMSG_SPACE(sizeof(int) * 4)];
	char recv_cmsgbuf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;
	char buf[10];
	int *fds;

	r = socketpair(PF_UNIX, SOCK_STREAM, 0, sock);
	if (r < 0)
		err(EXIT_FAILURE, "socketpair");

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = send_cmsgbuf;
	msg.msg_controllen = sizeof(send_cmsgbuf);
	msg.msg_flags = 0;

	iov.iov_base = (char *) "Hello";
	iov.iov_len = 5;

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int) * 4);
	fds = (int *)CMSG_DATA(cmsg);
	fds[0] = 0;
	fds[1] = 0;
	fds[2] = 0;
	fds[3] = 0;

	r = sendmsg(sock[0], &msg, 0);
	if (r < 0)
		err(EXIT_FAILURE, "sendmsg");

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = recv_cmsgbuf;
	msg.msg_controllen = sizeof(recv_cmsgbuf);

	iov.iov_base = buf;
	iov.iov_len = sizeof(buf) - 1;

	r = recvmsg(sock[1], &msg, 0);
	if (r < 0)
		err(EXIT_FAILURE, "recvmsg");

	assert(msg.msg_flags & MSG_CTRUNC);

	if (msg.msg_controllen > 0) {
		cmsg = CMSG_FIRSTHDR(&msg);
		if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
		    cmsg->cmsg_type == SCM_RIGHTS) {
			int nfds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);

			printf("received %d FDs\n", nfds);
		} else {
			printf("received cmsg other than SCM_RIGHTS\n");
		}
	} else {
		printf("no control data received\n");
	}
}));
