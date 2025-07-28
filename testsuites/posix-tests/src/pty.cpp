#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
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
