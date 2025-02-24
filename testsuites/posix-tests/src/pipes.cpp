#include <cassert>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>

#include "testsuite.hpp"

DEFINE_TEST(pipe_close_writer, ([] {
	int fds[2];
	int e = pipe(fds);
	assert(!e);
	close(fds[1]); // Close writer.

	pollfd pfd;
	memset(&pfd, 0, sizeof(pollfd));
	pfd.fd = fds[0];
	e = poll(&pfd, 1, 0); // Non-blocking poll().
	assert(e == 1);
	assert(!(pfd.revents & POLLIN));
	assert(!(pfd.revents & POLLERR));
	assert(pfd.revents & POLLHUP);
}))

DEFINE_TEST(pipe_close_reader, ([] {
	int fds[2];
	int e = pipe(fds);
	assert(!e);
	close(fds[0]); // Close reader.

	pollfd pfd;
	memset(&pfd, 0, sizeof(pollfd));
	pfd.fd = fds[1];
	e = poll(&pfd, 1, 0); // Non-blocking poll().
	assert(e == 1);
	assert(!(pfd.revents & POLLOUT));
	assert(pfd.revents & POLLERR);
	assert(!(pfd.revents & POLLHUP));
}))

DEFINE_TEST(fifo_rw, ([] {
	assert(mkfifo("/tmp/posix-testsuite-fifo", S_IRUSR | S_IWUSR) == 0);

	int fd = open("/tmp/posix-testsuite-fifo", O_RDWR | O_NONBLOCK);
	assert(fd >= 0);

	char buf[1] = {42};
	assert(write(fd, buf, 1) > 0);
	assert(read(fd, buf, 1) > 0);

	assert(close(fd) == 0);
	assert(unlink("/tmp/posix-testsuite-fifo") == 0);
}))
